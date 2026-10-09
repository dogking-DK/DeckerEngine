#include "GpuSimulation.hpp"
#include <dk/render/ClothRenderer.hpp>
#include <dk/memory/MemorySystem.hpp>

namespace dk::detail {
namespace {
template<class T, class E> T take(std::expected<T, E> value) {
    if (!value) {
        if constexpr (std::is_same_v<E, Error>) throw std::move(value.error());
        else throw std::bad_alloc{};
    }
    return std::move(*value);
}
void check(Result<void> value) { if (!value) throw std::move(value.error()); }
void wait(const GpuXpbdFrame& frame, graphics::SubmissionQueue& queue) {
    if (!take(frame.wait(queue))) throw Error{ErrorCode::invalid_state, "Simulation GPU wait did not complete"};
    if (queue.device().validation_errors()) throw Error{ErrorCode::internal_error, "Simulation Vulkan validation failed"};
}
}
struct GpuSimulation::Impl {
    memory::MemorySystem memory = take(memory::MemorySystem::create());
    memory::ResourceHandle heap = take(memory.create_heap({"simulation", memory::DomainCategory::render}));
    memory::ThreadContext context{memory, heap};
    std::optional<graphics::SubmissionQueue> queue;
    GpuXpbdSolver solver;
    std::optional<GpuXpbdFrame> pending;
    std::optional<render::ClothRenderer> renderer;
    ~Impl() { if (queue) { memory::ExecutionScope scope{context, heap}; (void)queue->close(); } }
};
GpuSimulation::GpuSimulation(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
GpuSimulation::~GpuSimulation() = default;
Result<std::shared_ptr<GpuSimulation>> GpuSimulation::create(const XpbdSolver& cpu, std::stop_token stop,
    std::shared_ptr<const graphics::Device> host_device, bool renderer_only) {
    try {
        if (stop.stop_requested()) return std::shared_ptr<GpuSimulation>{};
        auto impl = std::make_unique<Impl>();
        memory::ExecutionScope scope{impl->context, impl->heap};
        auto device = host_device ? host_device->share_queue(1) : graphics::Device::create(impl->heap, {.cancel=stop});
        if (!device && device.error().context == std::vector<std::string>{"graphics.device.create.cancelled"})
            return std::shared_ptr<GpuSimulation>{};
        if (!device) return std::unexpected(device.error().with_context("simulation.gpu.device"));
        if (stop.stop_requested()) return std::shared_ptr<GpuSimulation>{};
        impl->queue.emplace(take(graphics::SubmissionQueue::create(impl->heap, std::move(*device))));
        if (!renderer_only) {
        auto solver = GpuXpbdSolver::create(impl->heap, *impl->queue, cpu, DK_XPBD_SHADER, stop);
        if (!solver && solver.error().context == std::vector<std::string>{"physics.gpu.initialize.cancelled"})
            return std::shared_ptr<GpuSimulation>{};
        impl->solver = take(std::move(solver));
        }
        if (stop.stop_requested()) return std::shared_ptr<GpuSimulation>{};
        return std::shared_ptr<GpuSimulation>(new GpuSimulation(std::move(impl)));
    } catch (Error& e) { return std::unexpected(std::move(e)); }
}
std::uint64_t GpuSimulation::steps() const { return impl_->solver.steps(); }
Result<void> GpuSimulation::step(std::int64_t dt, std::uint32_t count) {
    auto& s = *impl_; memory::ExecutionScope scope{s.context, s.heap};
    try { auto frame = take(s.solver.advance(*s.queue, dt, count)); wait(frame, *s.queue); return {}; }
    catch (Error& e) { return std::unexpected(std::move(e)); }
}
Result<bool> GpuSimulation::submit(std::int64_t dt, std::uint32_t count, std::stop_token stop) {
    auto& s = *impl_; memory::ExecutionScope scope{s.context, s.heap};
    if (s.pending) return std::unexpected(Error{ErrorCode::invalid_state,"Simulation already has an in-flight batch"});
    auto frame = s.solver.advance(*s.queue,dt,count,{.cancel=stop});
    if (!frame && (frame.error().context==std::vector<std::string>{"graph.compile.cancelled"} ||
                   frame.error().context==std::vector<std::string>{"physics.gpu.advance.cancelled"})) return false;
    if (!frame) return std::unexpected(frame.error());
    s.pending = std::move(*frame);
    return true;
}
Result<bool> GpuSimulation::poll() {
    auto& s = *impl_; memory::ExecutionScope scope{s.context, s.heap};
    if (!s.pending) return true;
    auto completed = s.pending->wait(*s.queue,0);
    if (!completed) return std::unexpected(completed.error());
    if (!*completed) return false;
    s.pending.reset();
    if (s.queue->device().validation_errors())
        return std::unexpected(Error{ErrorCode::internal_error,"Simulation Vulkan validation failed"});
    return true;
}
Result<GpuParticles> GpuSimulation::read(std::int64_t dt) {
    auto& s = *impl_; memory::ExecutionScope scope{s.context, s.heap};
    try {
        auto frame = take(s.solver.advance(*s.queue, dt, 0, {.readback = true}));
        wait(frame, *s.queue); return frame.read_particles();
    } catch (Error& e) { return std::unexpected(std::move(e)); }
}
Result<SimulationImage> GpuSimulation::capture(std::int64_t dt, const ClothConfig& cloth, std::uint32_t width, std::uint32_t height) {
    auto& s = *impl_; memory::ExecutionScope scope{s.context, s.heap};
    try {
        if (!s.renderer) s.renderer.emplace(take(render::ClothRenderer::create(s.heap, *s.queue, DK_CLOTH_SHADER)));
        render::ClothView view;
        view.columns = cloth.columns; view.rows = cloth.rows; view.width = width; view.height = height;
        view.floor_y = cloth.physics.floor_y; view.image_readback = true;
        auto frame = take(s.renderer->render(*s.queue, s.solver, dt, 0, view, {.readback = true}));
        wait(frame.physics(), *s.queue);
        SimulationImage result{take(frame.physics().read_particles()), std::vector<std::byte>(std::size_t{width}*height*4)};
        check(frame.read_rgba8(result.rgba));
        return result;
    } catch (Error& e) { return std::unexpected(std::move(e)); }
}
Result<graphics::ImageTransfer> GpuSimulation::preview(std::int64_t dt,const ClothConfig& cloth,const SimulationView& request,
    std::span<const ParticlePosition> cpu_positions) {
    auto& s = *impl_; memory::ExecutionScope scope{s.context,s.heap};
    try {
        if (!s.renderer) s.renderer.emplace(take(render::ClothRenderer::create(s.heap,*s.queue,DK_CLOTH_SHADER)));
        render::ClothView view;
        view.columns=cloth.columns; view.rows=cloth.rows; view.width=request.width; view.height=request.height;
        view.floor_y=cloth.physics.floor_y; view.view_projection=request.view_projection;
        auto frame=cpu_positions.empty() ? take(s.renderer->render(*s.queue,s.solver,dt,0,view)) :
            take(s.renderer->render_positions(*s.queue,cpu_positions,view));
        if (!take(frame.wait(*s.queue))) throw Error{ErrorCode::invalid_state,"Preview did not complete"};
        return s.queue->export_image(*take(frame.color()));
    } catch (Error& e) { return std::unexpected(std::move(e)); }
}
}

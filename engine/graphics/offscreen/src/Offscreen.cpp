#include "OffscreenPolicy.hpp"
#include <dk/graphics/Transfer.hpp>
#include <dk/profiling/Profiler.hpp>
#include <utility>

namespace dk::graphics {
namespace {
struct Work {
    explicit Work(memory::ResourceHandle resource) : readbacks(memory::Allocator<ReadbackRequest>{resource}) {}
    Vector<ReadbackRequest> readbacks;
    Submission ticket;
};
} // namespace

struct OffscreenExecutor::Impl {
    Impl(memory::ResourceHandle memory, SubmissionQueue&& submission_queue)
        : resource(std::move(memory)), queue(std::move(submission_queue)) {}
    memory::ResourceHandle resource;
    // Queue completion updates independent readback records before their final release.
    memory::UniquePtr<Work> pending;
    SubmissionQueue queue;
    Result<void> accepting() const
    {
        if (queue.stats().device_lost || queue.stats().closed)
            return std::unexpected(Error{ErrorCode::invalid_state, "offscreen executor is closed or device lost"});
        if (resource.state() != memory::ResourceState::open)
            return std::unexpected(Error{ErrorCode::invalid_state, "offscreen Memory resource is closing or closed"});
        if (pending) return std::unexpected(Error{ErrorCode::conflict, "offscreen work is pending; drain before starting another operation"});
        return {};
    }
    Result<void> execute(memory::UniquePtr<Work> work, CommandBatch&& batch, std::uint64_t timeout)
    {
        pending = std::move(work); // Reserve result requests and the ticket slot before submission.
        // SubmissionQueue cannot throw after a successful native submit. An exception
        // before that commit must not strand an empty ticket in pending.
        auto ticket = [&]() {
            try { return queue.submit(std::move(batch)); }
            catch (...) { pending.reset(); throw; }
        }();
        if (!ticket) { pending.reset(); return std::unexpected(ticket.error()); }
        static_assert(std::is_nothrow_move_assignable_v<Submission>);
        pending->ticket = std::move(*ticket); // No allocation after the submit commit.
        auto completed = queue.wait(pending->ticket, timeout);
        if (!completed) return std::unexpected(completed.error());
        if (!*completed) return std::unexpected(Error{ErrorCode::conflict, "offscreen wait timed out; GPU resources retained until drain"});
        return {};
    }

};

OffscreenExecutor::OffscreenExecutor(memory::UniquePtr<Impl> impl) : impl_(std::move(impl)) {}
OffscreenExecutor::~OffscreenExecutor() = default;
OffscreenExecutor::OffscreenExecutor(OffscreenExecutor&&) noexcept = default;
OffscreenExecutor& OffscreenExecutor::operator=(OffscreenExecutor&&) noexcept = default;
Result<OffscreenExecutor> OffscreenExecutor::create(memory::ResourceHandle resource, Device&& device)
{
    auto queue = SubmissionQueue::create(resource, std::move(device), 1);
    if (!queue) return std::unexpected(queue.error());
    return detail::OffscreenAccess::create(std::move(resource), std::move(*queue));
}
Result<OffscreenExecutor> detail::OffscreenAccess::create(memory::ResourceHandle resource, SubmissionQueue&& queue)
{
    if (!resource || resource.state() != memory::ResourceState::open)
        return std::unexpected(Error{ErrorCode::invalid_argument, "offscreen executor requires open Memory resource"});
    return OffscreenExecutor{memory::make_unique_in<OffscreenExecutor::Impl>(resource, resource, std::move(queue))};
}
const Device& OffscreenExecutor::device() const noexcept { return impl_->queue.device(); }
SubmissionStats OffscreenExecutor::stats() const noexcept { return impl_->queue.stats(); }
Result<bool> OffscreenExecutor::drain(std::uint64_t timeout_ns)
{
    if (!impl_) return std::unexpected(Error{ErrorCode::invalid_state, "offscreen executor is moved from"});
    if (!impl_->pending) return true;
    auto completed = impl_->queue.wait(impl_->pending->ticket, timeout_ns);
    if (completed && *completed) impl_->pending.reset();
    return completed;
}

Result<OffscreenImage> OffscreenExecutor::draw(const CompiledShader& vertex, const CompiledShader& fragment,
    const OffscreenDraw& description, std::uint64_t timeout_ns)
{
    DK_PROFILE_ZONE("offscreen.draw");
    if (!impl_) return std::unexpected(Error{ErrorCode::invalid_state, "offscreen executor is moved from"});
    if (auto status = impl_->accepting(); !status) return std::unexpected(status.error());
    const auto size = detail::validate_draw(vertex, fragment, description, device().adapter().properties.limits);
    if (!size) return std::unexpected(size.error());
    auto& queue = impl_->queue;
    auto factory = queue.resources();
    OffscreenImage output{description.width, description.height, Vector<std::byte>{memory::Allocator<std::byte>{impl_->resource}}};
    output.rgba8.resize(*size);
    auto work = memory::make_unique_in<Work>(impl_->resource,impl_->resource);
    work->readbacks.reserve(1);
    auto vs = factory.create_shader(vertex);
    if (!vs) return std::unexpected(vs.error());
    auto fs = factory.create_shader(fragment);
    if (!fs) return std::unexpected(fs.error());
    const std::array shaders{&*vs,&*fs};
    auto layout = factory.create_pipeline_layout(shaders);
    if (!layout) return std::unexpected(layout.error());
    auto pipeline = factory.create_graphics_pipeline({&*vs,&*fs,&*layout});
    if (!pipeline) return std::unexpected(pipeline.error());
    auto image = factory.create_image({description.width,description.height,vk::Format::eR8G8B8A8Unorm,
        vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eTransferSrc});
    if (!image) return std::unexpected(image.error());
    auto view = factory.create_view(*image);
    if (!view) return std::unexpected(view.error());
    auto batch = queue.begin();
    if (!batch) return std::unexpected(batch.error());
    const std::array uses{image_use(*image,vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        vk::AccessFlagBits2::eColorAttachmentRead | vk::AccessFlagBits2::eColorAttachmentWrite,vk::ImageLayout::eColorAttachmentOptimal)};
    if (auto status = batch->prepare(uses); !status) return std::unexpected(status.error());
    RenderingDesc rendering{};
    rendering.color.view = &*view;
    rendering.color.clear = description.clear_color;
    auto render = batch->begin_rendering(rendering);
    if (!render) return std::unexpected(render.error());
    if (auto status = render->bind_pipeline(*pipeline); !status) return std::unexpected(status.error());
    if (auto status = render->draw(description.vertex_count); !status) return std::unexpected(status.error());
    if (auto status = render->end(); !status) return std::unexpected(status.error());
    auto request = batch->readback(*image,{0,0,0,0,description.width,description.height});
    if (!request) return std::unexpected(request.error());
    work->readbacks.push_back(std::move(*request));
    if (auto status = impl_->execute(std::move(work),std::move(*batch),timeout_ns); !status) return std::unexpected(status.error());
    auto status = impl_->pending->readbacks.front().try_read(output.rgba8);
    impl_->pending.reset();
    if (!status) return std::unexpected(status.error());
    if (!*status) return std::unexpected(Error{ErrorCode::internal_error,"completed offscreen readback is not ready"});
    return output;
}

Result<ComputeOutput> OffscreenExecutor::dispatch(const CompiledShader& shader, std::span<const ComputeBufferInput> buffers,
    std::span<const std::byte> push_constants, std::array<std::uint32_t,3> groups, std::uint64_t timeout_ns)
{
    DK_PROFILE_ZONE("offscreen.dispatch");
    if (!impl_) return std::unexpected(Error{ErrorCode::invalid_state,"offscreen executor is moved from"});
    if (auto status = impl_->accepting(); !status) return std::unexpected(status.error());
    if (auto status = detail::validate_dispatch(shader,buffers,push_constants,groups,device().adapter().properties.limits); !status)
        return std::unexpected(status.error());
    auto& queue = impl_->queue;
    auto factory = queue.resources();
    ComputeOutput output{memory::Allocator<ComputeBufferOutput>{impl_->resource}};
    Vector<Buffer> storage{memory::Allocator<Buffer>{impl_->resource}};
    Vector<BindingWrite> writes{memory::Allocator<BindingWrite>{impl_->resource}};
    Vector<ResourceUse> uses{memory::Allocator<ResourceUse>{impl_->resource}};
    auto work = memory::make_unique_in<Work>(impl_->resource,impl_->resource);
    output.reserve(buffers.size());
    storage.reserve(buffers.size()); // Keep addresses used by descriptors/access declarations stable.
    writes.reserve(buffers.size());
    uses.reserve(buffers.size());
    work->readbacks.reserve(buffers.size());
    auto module = factory.create_shader(shader);
    if (!module) return std::unexpected(module.error());
    const std::array shaders{&*module};
    auto layout = factory.create_pipeline_layout(shaders);
    if (!layout) return std::unexpected(layout.error());
    auto pipeline = factory.create_compute_pipeline({&*module,&*layout});
    if (!pipeline) return std::unexpected(pipeline.error());
    auto batch = queue.begin();
    if (!batch) return std::unexpected(batch.error());
    for (const auto& input : buffers) {
        output.push_back({input.binding,Vector<std::byte>{memory::Allocator<std::byte>{impl_->resource}}});
        output.back().bytes.resize(input.bytes.size());
        auto buffer = factory.create_buffer({input.bytes.size(),vk::BufferUsageFlagBits::eStorageBuffer |
            vk::BufferUsageFlagBits::eTransferSrc | vk::BufferUsageFlagBits::eTransferDst});
        if (!buffer) return std::unexpected(buffer.error());
        storage.push_back(std::move(*buffer));
        if (auto status = batch->upload(storage.back(),input.bytes); !status) return std::unexpected(status.error());
        writes.push_back({input.binding,0,BufferBinding{&storage.back()}});
        uses.push_back(buffer_use(storage.back(),vk::PipelineStageFlagBits2::eComputeShader,
            vk::AccessFlagBits2::eShaderStorageRead | vk::AccessFlagBits2::eShaderStorageWrite));
    }
    auto bindings = factory.create_bindings(*layout,0,writes);
    if (!bindings) return std::unexpected(bindings.error());
    if (auto status = batch->prepare(uses); !status) return std::unexpected(status.error());
    const std::array sets{&*bindings};
    if (auto status = batch->dispatch(*pipeline,sets,push_constants,groups); !status) return std::unexpected(status.error());
    for (const auto& buffer : storage) {
        auto request = batch->readback(buffer);
        if (!request) return std::unexpected(request.error());
        work->readbacks.push_back(std::move(*request));
    }
    if (auto status = impl_->execute(std::move(work),std::move(*batch),timeout_ns); !status) return std::unexpected(status.error());
    for (std::size_t i = 0; i < buffers.size(); ++i) {
        auto status = impl_->pending->readbacks[i].try_read(output[i].bytes);
        if (!status || !*status) {
            impl_->pending.reset();
            return std::unexpected(status ? Error{ErrorCode::internal_error,"completed compute readback is not ready"} : status.error());
        }
    }
    impl_->pending.reset();
    return output;
}
} // namespace dk::graphics

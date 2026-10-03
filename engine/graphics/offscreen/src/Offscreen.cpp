#include "OffscreenPolicy.hpp"
#include <dk/graphics/GraphExecution.hpp>
#include <dk/profiling/Profiler.hpp>
#include <utility>

namespace dk::graphics {
namespace {
struct Work { graph::Execution execution; };
using graph::Use;
Use read(graph::ResourceId id, vk::PipelineStageFlags2 stage, vk::AccessFlags2 access,
    vk::ImageLayout layout = vk::ImageLayout::eUndefined) { return {id,{{stage,access,layout}}}; }
Use write(graph::ResourceId id, vk::PipelineStageFlags2 stage, vk::AccessFlags2 access,
    vk::ImageLayout layout = vk::ImageLayout::eUndefined) { return {id,{{stage,access,layout},0,VK_WHOLE_SIZE,{vk::ImageAspectFlagBits::eColor,0,1,0,1},true}}; }
} // namespace

struct OffscreenExecutor::Impl {
    Impl(memory::ResourceHandle memory, SubmissionQueue&& submission_queue)
        : resource(std::move(memory)), queue(std::move(submission_queue)) {}
    memory::ResourceHandle resource;
    // Destroy/drain the queue before releasing Graph output owners.
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
    Result<void> execute(const graph::CompiledGraph& plan, const graph::ExecutionDesc& desc, std::uint64_t timeout)
    {
        pending = memory::make_unique_in<Work>(resource); // Reserve before Graph's submit commit.
        auto result = [&]() {
            try { return graph::execute(plan,queue,desc); }
            catch (...) { pending.reset(); throw; }
        }();
        if (!result) { pending.reset(); return std::unexpected(result.error()); }
        static_assert(std::is_nothrow_move_assignable_v<graph::Execution>);
        pending->execution = std::move(*result);
        auto completed = queue.wait(pending->execution.submission(), timeout);
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
    auto completed = impl_->queue.wait(impl_->pending->execution.submission(), timeout_ns);
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
    auto vs = factory.create_shader(vertex);
    if (!vs) return std::unexpected(vs.error());
    auto fs = factory.create_shader(fragment);
    if (!fs) return std::unexpected(fs.error());
    const std::array shaders{&*vs,&*fs};
    auto layout = factory.create_pipeline_layout(shaders);
    if (!layout) return std::unexpected(layout.error());
    auto pipeline = factory.create_graphics_pipeline({&*vs,&*fs,&*layout});
    if (!pipeline) return std::unexpected(pipeline.error());
    auto declared = graph::Graph::create(impl_->resource); if (!declared) return std::unexpected(declared.error());
    auto& graph = *declared;
    auto readback = graph.declare_buffer("readback",{*size,vk::BufferUsageFlagBits::eTransferDst,BufferMemory::readback});
    if (!readback) return std::unexpected(readback.error());
    auto color = graph.declare_image("color",{description.width,description.height,vk::Format::eR8G8B8A8Unorm,
        vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst});
    if (!color) return std::unexpected(color.error());
    const std::array clear_uses{write(*color,vk::PipelineStageFlagBits2::eClear,vk::AccessFlagBits2::eTransferWrite,vk::ImageLayout::eTransferDstOptimal)};
    const std::array draw_uses{read(*color,vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        vk::AccessFlagBits2::eColorAttachmentRead | vk::AccessFlagBits2::eColorAttachmentWrite,vk::ImageLayout::eColorAttachmentOptimal)};
    const std::array copy_uses{read(*color,vk::PipelineStageFlagBits2::eCopy,vk::AccessFlagBits2::eTransferRead,vk::ImageLayout::eTransferSrcOptimal),
        write(*readback,vk::PipelineStageFlagBits2::eCopy,vk::AccessFlagBits2::eTransferWrite)};
    if (auto added = graph.add_pass({"clear",clear_uses}); !added) return std::unexpected(added.error());
    if (auto added = graph.add_pass({"draw",draw_uses}); !added) return std::unexpected(added.error());
    if (auto added = graph.add_pass({"readback",copy_uses}); !added) return std::unexpected(added.error());
    if (auto marked = graph.mark_output(*readback); !marked) return std::unexpected(marked.error());
    auto plan = graph.compile(); if (!plan) return std::unexpected(plan.error());
    struct Context { ResourceFactory& factory; GraphicsPipeline& pipeline; const OffscreenDraw& draw; } context{factory,*pipeline,description};
    const std::array callbacks{
        graph::PassCallback{0,[](graph::PassContext& pass,void* data) {
            return pass.clear(1,vk::ClearColorValue{static_cast<Context*>(data)->draw.clear_color},{vk::ImageAspectFlagBits::eColor,0,1,0,1});
        },&context},
        graph::PassCallback{1,[](graph::PassContext& pass,void* data) -> Result<void> {
            auto& c = *static_cast<Context*>(data);
            auto image = pass.image(1); if (!image) return std::unexpected(image.error());
            auto view = c.factory.create_view(**image); if (!view) return std::unexpected(view.error());
            RenderingDesc rendering{}; rendering.color.view = &*view; rendering.color.load = vk::AttachmentLoadOp::eLoad;
            auto render = pass.begin_rendering(rendering); if (!render) return std::unexpected(render.error());
            if (auto status = render->bind_pipeline(c.pipeline); !status) return status;
            if (auto status = render->draw(c.draw.vertex_count); !status) return status;
            return render->end();
        },&context},
        graph::PassCallback{2,[](graph::PassContext& pass,void* data) {
            const auto& draw = static_cast<Context*>(data)->draw;
            return pass.copy_to_buffer(1,0,{0,0,0,0,draw.width,draw.height});
        },&context}};
    const std::array final{graph::FinalAccess{0,{{vk::PipelineStageFlagBits2::eHost,vk::AccessFlagBits2::eHostRead}}}};
    if (auto status = impl_->execute(*plan,{{},callbacks,final},timeout_ns); !status) return std::unexpected(status.error());
    auto result = impl_->pending->execution.buffer(0);
    auto status = result ? (*result)->read(0,output.rgba8) : Result<void>{std::unexpected(result.error())};
    impl_->pending.reset();
    if (!status) return std::unexpected(status.error());
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
    Vector<Buffer> uploads(0,memory::Allocator<Buffer>{impl_->resource});
    Vector<graph::ExternalBinding> external(0,memory::Allocator<graph::ExternalBinding>{impl_->resource});
    Vector<Use> upload_uses(0,memory::Allocator<Use>{impl_->resource}), compute_uses(0,memory::Allocator<Use>{impl_->resource}),
        readback_uses(0,memory::Allocator<Use>{impl_->resource});
    Vector<graph::FinalAccess> final(0,memory::Allocator<graph::FinalAccess>{impl_->resource});
    output.reserve(buffers.size()); uploads.reserve(buffers.size()); external.reserve(buffers.size());
    auto declared = graph::Graph::create(impl_->resource); if (!declared) return std::unexpected(declared.error());
    auto& graph = *declared;
    auto module = factory.create_shader(shader);
    if (!module) return std::unexpected(module.error());
    const std::array shaders{&*module};
    auto layout = factory.create_pipeline_layout(shaders);
    if (!layout) return std::unexpected(layout.error());
    auto pipeline = factory.create_compute_pipeline({&*module,&*layout});
    if (!pipeline) return std::unexpected(pipeline.error());
    for (std::size_t i=0; i<buffers.size(); ++i) {
        const auto& input = buffers[i];
        output.push_back({input.binding,Vector<std::byte>{memory::Allocator<std::byte>{impl_->resource}}});
        output.back().bytes.resize(input.bytes.size());
        const BufferDesc upload_desc{input.bytes.size(),vk::BufferUsageFlagBits::eTransferSrc,BufferMemory::upload};
        auto upload = factory.create_buffer(upload_desc); if (!upload) return std::unexpected(upload.error());
        if (auto status = upload->write(0,input.bytes); !status) return std::unexpected(status.error());
        uploads.push_back(std::move(*upload));
        const auto suffix = std::to_string(input.binding);
        auto source = graph.declare_buffer("upload-"+suffix,upload_desc,graph::Lifetime::external,true);
        if (!source) return std::unexpected(source.error());
        auto storage = graph.declare_buffer("storage-"+suffix,{input.bytes.size(),vk::BufferUsageFlagBits::eStorageBuffer |
            vk::BufferUsageFlagBits::eTransferSrc | vk::BufferUsageFlagBits::eTransferDst});
        if (!storage) return std::unexpected(storage.error());
        auto result = graph.declare_buffer("readback-"+suffix,{input.bytes.size(),vk::BufferUsageFlagBits::eTransferDst,BufferMemory::readback});
        if (!result) return std::unexpected(result.error());
        external.push_back({3*i,&uploads.back()});
        upload_uses.push_back(read(*source,vk::PipelineStageFlagBits2::eCopy,vk::AccessFlagBits2::eTransferRead));
        upload_uses.push_back(write(*storage,vk::PipelineStageFlagBits2::eCopy,vk::AccessFlagBits2::eTransferWrite));
        compute_uses.push_back(read(*storage,vk::PipelineStageFlagBits2::eComputeShader,
            vk::AccessFlagBits2::eShaderStorageRead | vk::AccessFlagBits2::eShaderStorageWrite));
        readback_uses.push_back(read(*storage,vk::PipelineStageFlagBits2::eCopy,vk::AccessFlagBits2::eTransferRead));
        readback_uses.push_back(write(*result,vk::PipelineStageFlagBits2::eCopy,vk::AccessFlagBits2::eTransferWrite));
        final.push_back({3*i+2,{{vk::PipelineStageFlagBits2::eHost,vk::AccessFlagBits2::eHostRead}}});
        if (auto marked = graph.mark_output(*result); !marked) return std::unexpected(marked.error());
    }
    if (auto added = graph.add_pass({"upload",upload_uses}); !added) return std::unexpected(added.error());
    if (auto added = graph.add_pass({"compute",compute_uses}); !added) return std::unexpected(added.error());
    if (auto added = graph.add_pass({"readback",readback_uses}); !added) return std::unexpected(added.error());
    auto plan = graph.compile(); if (!plan) return std::unexpected(plan.error());
    struct Context {
        ResourceFactory& factory; ComputePipeline& pipeline; PipelineLayout& layout;
        std::span<const ComputeBufferInput> buffers; std::span<const std::byte> constants;
        std::array<std::uint32_t,3> groups; memory::ResourceHandle heap;
    } context{factory,*pipeline,*layout,buffers,push_constants,groups,impl_->resource};
    const std::array callbacks{
        graph::PassCallback{0,[](graph::PassContext& pass,void* data) -> Result<void> {
            const auto& c = *static_cast<Context*>(data);
            for (std::size_t i=0; i<c.buffers.size(); ++i)
                if (auto status = pass.copy_buffer(3*i,3*i+1,c.buffers[i].bytes.size()); !status) return status;
            return {};
        },&context},
        graph::PassCallback{1,[](graph::PassContext& pass,void* data) -> Result<void> {
            auto& c = *static_cast<Context*>(data);
            Vector<BindingWrite> writes(0,memory::Allocator<BindingWrite>{c.heap});
            for (std::size_t i=0; i<c.buffers.size(); ++i) {
                auto buffer = pass.buffer(3*i+1); if (!buffer) return std::unexpected(buffer.error());
                writes.push_back({c.buffers[i].binding,0,BufferBinding{*buffer}});
            }
            auto bindings = c.factory.create_bindings(c.layout,0,writes); if (!bindings) return std::unexpected(bindings.error());
            auto encoder = pass.compute(); if (!encoder) return std::unexpected(encoder.error());
            if (auto status = encoder->bind_pipeline(c.pipeline); !status) return status;
            const std::array sets{&*bindings};
            if (auto status = encoder->bind_sets(sets); !status) return status;
            if (!c.constants.empty()) if (auto status = encoder->push_constants(vk::ShaderStageFlagBits::eCompute,0,c.constants); !status) return status;
            return encoder->dispatch(c.groups);
        },&context},
        graph::PassCallback{2,[](graph::PassContext& pass,void* data) -> Result<void> {
            const auto& c = *static_cast<Context*>(data);
            for (std::size_t i=0; i<c.buffers.size(); ++i)
                if (auto status = pass.copy_buffer(3*i+1,3*i+2,c.buffers[i].bytes.size()); !status) return status;
            return {};
        },&context}};
    if (auto status = impl_->execute(*plan,{external,callbacks,final},timeout_ns); !status) return std::unexpected(status.error());
    for (std::size_t i=0; i<buffers.size(); ++i) {
        auto buffer = impl_->pending->execution.buffer(3*i+2);
        auto status = buffer ? (*buffer)->read(0,output[i].bytes) : Result<void>{std::unexpected(buffer.error())};
        if (!status) { impl_->pending.reset(); return std::unexpected(status.error()); }
    }
    impl_->pending.reset();
    return output;
}
} // namespace dk::graphics

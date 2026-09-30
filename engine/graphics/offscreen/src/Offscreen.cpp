#include "OffscreenPolicy.hpp"
#include <dk/profiling/Profiler.hpp>
#include <utility>

namespace dk::graphics {
namespace {
struct Work {
    explicit Work(memory::ResourceHandle resource)
        : uploads(memory::Allocator<Buffer>{resource}), storage(memory::Allocator<Buffer>{resource}),
          readbacks(memory::Allocator<Buffer>{resource}) {}
    // Buffers/images retain Device and must outlive all borrowed-dispatcher RAII objects.
    Vector<Buffer> uploads, storage, readbacks;
    Image image;
    vk::raii::ShaderModule first_shader{nullptr}, second_shader{nullptr};
    vk::raii::DescriptorSetLayout descriptor_layout{nullptr};
    vk::raii::PipelineLayout layout{nullptr};
    vk::raii::DescriptorPool pool{nullptr};
    vk::raii::DescriptorSet descriptor_set{nullptr};
    vk::raii::ImageView view{nullptr};
    vk::raii::Pipeline pipeline{nullptr};
    Submission ticket;
};
vk::raii::ShaderModule module(const Device& device, const CompiledShader& shader)
{
    vk::ShaderModuleCreateInfo info{};
    info.setCode(shader.spirv);
    return {device.logical_device(), info};
}
Error vk_error(const vk::SystemError& error)
{
    return {ErrorCode::internal_error, std::string{"offscreen Vulkan operation failed: "} + error.what()};
}
} // namespace

struct OffscreenExecutor::Impl {
    Impl(memory::ResourceHandle memory, SubmissionQueue&& submission_queue)
        : resource(std::move(memory)), queue(std::move(submission_queue)) {}
    memory::ResourceHandle resource;
    // Reverse destruction: queue first drains/falls back to idle, then pending RAII.
    // Work's VMA owners preserve Device after QueueState itself has been destroyed.
    memory::UniquePtr<Work> pending;
    SubmissionQueue queue;
    bool failed = false;
    Result<void> accepting() const
    {
        if (failed || queue.stats().device_lost || queue.stats().closed)
            return std::unexpected(Error{ErrorCode::invalid_state, "offscreen executor is closed or device lost"});
        if (resource.state() != memory::ResourceState::open)
            return std::unexpected(Error{ErrorCode::invalid_state, "offscreen Memory resource is closing or closed"});
        if (pending) return std::unexpected(Error{ErrorCode::conflict, "offscreen work is pending; drain before starting another operation"});
        return {};
    }
    Result<void> execute(memory::UniquePtr<Work> work, CommandBatch&& batch, std::uint64_t timeout)
    {
        pending = std::move(work); // Reserve all non-VMA GPU owners before submission.
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
    Error failure(const vk::SystemError& error)
    {
        if (error.code().value() == static_cast<int>(vk::Result::eErrorDeviceLost)) failed = true;
        return vk_error(error);
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
    const auto& logical = device().logical_device();
    OffscreenImage output{description.width, description.height, Vector<std::byte>{memory::Allocator<std::byte>{impl_->resource}}};
    output.rgba8.resize(*size); // All return storage exists before any GPU submission.
    auto work = memory::make_unique_in<Work>(impl_->resource, impl_->resource);
    work->readbacks.reserve(1);
    auto readback = queue.create_buffer({*size, vk::BufferUsageFlagBits::eTransferDst, BufferMemory::readback});
    if (!readback) return std::unexpected(readback.error());
    work->readbacks.push_back(std::move(*readback));
    auto image = queue.create_image({description.width, description.height, vk::Format::eR8G8B8A8Unorm,
        vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eTransferSrc});
    if (!image) return std::unexpected(image.error());
    work->image = std::move(*image);
    try {
        work->first_shader = module(device(), vertex);
        work->second_shader = module(device(), fragment);
        work->layout = vk::raii::PipelineLayout{logical, vk::PipelineLayoutCreateInfo{}};
        work->view = vk::raii::ImageView{logical, vk::ImageViewCreateInfo{{}, work->image.handle(), vk::ImageViewType::e2D,
            vk::Format::eR8G8B8A8Unorm, {}, vk::ImageSubresourceRange{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}}};
        const std::array stages{
            vk::PipelineShaderStageCreateInfo{{}, vk::ShaderStageFlagBits::eVertex, *work->first_shader, vertex.entry.c_str()},
            vk::PipelineShaderStageCreateInfo{{}, vk::ShaderStageFlagBits::eFragment, *work->second_shader, fragment.entry.c_str()}};
        const vk::PipelineVertexInputStateCreateInfo input{};
        const vk::PipelineInputAssemblyStateCreateInfo assembly{{}, vk::PrimitiveTopology::eTriangleList, VK_FALSE};
        vk::PipelineViewportStateCreateInfo viewport{};
        viewport.viewportCount = viewport.scissorCount = 1;
        vk::PipelineRasterizationStateCreateInfo raster{};
        raster.polygonMode = vk::PolygonMode::eFill;
        raster.cullMode = vk::CullModeFlagBits::eNone;
        raster.frontFace = vk::FrontFace::eCounterClockwise;
        raster.lineWidth = 1.0F;
        const vk::PipelineMultisampleStateCreateInfo samples{{}, vk::SampleCountFlagBits::e1};
        vk::PipelineColorBlendAttachmentState attachment{};
        attachment.colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG
            | vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;
        vk::PipelineColorBlendStateCreateInfo blend{};
        blend.setAttachments(attachment);
        const std::array dynamic_states{vk::DynamicState::eViewport, vk::DynamicState::eScissor};
        vk::PipelineDynamicStateCreateInfo dynamic{};
        dynamic.setDynamicStates(dynamic_states);
        const vk::Format format = vk::Format::eR8G8B8A8Unorm;
        vk::PipelineRenderingCreateInfo rendering{};
        rendering.setColorAttachmentFormats(format);
        vk::GraphicsPipelineCreateInfo pipeline{};
        pipeline.setStages(stages).setPVertexInputState(&input).setPInputAssemblyState(&assembly)
            .setPViewportState(&viewport).setPRasterizationState(&raster).setPMultisampleState(&samples)
            .setPColorBlendState(&blend).setPDynamicState(&dynamic).setLayout(*work->layout).setPNext(&rendering);
        work->pipeline = vk::raii::Pipeline{logical, nullptr, pipeline};
        auto batch = queue.begin();
        if (!batch) return std::unexpected(batch.error());
        if (auto status = batch->transition(work->image, vk::ImageLayout::eColorAttachmentOptimal); !status)
            return std::unexpected(status.error());
        vk::RenderingAttachmentInfo color{};
        color.imageView = *work->view;
        color.imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
        color.loadOp = vk::AttachmentLoadOp::eClear;
        color.storeOp = vk::AttachmentStoreOp::eStore;
        color.clearValue.color = vk::ClearColorValue{description.clear_color};
        vk::RenderingInfo info{};
        info.renderArea = vk::Rect2D{{0, 0}, {description.width, description.height}};
        info.layerCount = 1;
        info.setColorAttachments(color);
        const auto& command = batch->command_buffer();
        command.beginRendering(info);
        command.bindPipeline(vk::PipelineBindPoint::eGraphics, *work->pipeline);
        command.setViewport(0, vk::Viewport{0, 0, static_cast<float>(description.width), static_cast<float>(description.height), 0, 1});
        command.setScissor(0, info.renderArea);
        command.draw(description.vertex_count, 1, 0, 0);
        command.endRendering();
        if (auto status = batch->copy_to_buffer(work->image, work->readbacks.front()); !status) return std::unexpected(status.error());
        if (auto status = impl_->execute(std::move(work), std::move(*batch), timeout_ns); !status) return std::unexpected(status.error());
        auto status = impl_->pending->readbacks.front().read(0, output.rgba8);
        impl_->pending.reset();
        if (!status) return std::unexpected(status.error());
        return output;
    } catch (const vk::SystemError& error) { return std::unexpected(impl_->failure(error)); }
}

Result<ComputeOutput> OffscreenExecutor::dispatch(const CompiledShader& shader, std::span<const ComputeBufferInput> buffers,
    std::span<const std::byte> push_constants, std::array<std::uint32_t, 3> groups, std::uint64_t timeout_ns)
{
    DK_PROFILE_ZONE("offscreen.dispatch");
    if (!impl_) return std::unexpected(Error{ErrorCode::invalid_state, "offscreen executor is moved from"});
    if (auto status = impl_->accepting(); !status) return std::unexpected(status.error());
    if (auto status = detail::validate_dispatch(shader, buffers, push_constants, groups, device().adapter().properties.limits); !status)
        return std::unexpected(status.error());
    auto& queue = impl_->queue;
    const auto& logical = device().logical_device();
    ComputeOutput output{memory::Allocator<ComputeBufferOutput>{impl_->resource}};
    output.reserve(buffers.size());
    auto work = memory::make_unique_in<Work>(impl_->resource, impl_->resource);
    work->uploads.reserve(buffers.size());
    work->storage.reserve(buffers.size());
    work->readbacks.reserve(buffers.size());
    for (const auto& input : buffers) {
        output.push_back({input.binding, Vector<std::byte>{memory::Allocator<std::byte>{impl_->resource}}});
        output.back().bytes.resize(input.bytes.size());
        auto upload = queue.create_buffer({input.bytes.size(), vk::BufferUsageFlagBits::eTransferSrc, BufferMemory::upload});
        if (!upload) return std::unexpected(upload.error());
        if (auto status = upload->write(0, input.bytes); !status) return std::unexpected(status.error());
        work->uploads.push_back(std::move(*upload));
        auto storage = queue.create_buffer({input.bytes.size(), vk::BufferUsageFlagBits::eStorageBuffer
            | vk::BufferUsageFlagBits::eTransferSrc | vk::BufferUsageFlagBits::eTransferDst, BufferMemory::device});
        if (!storage) return std::unexpected(storage.error());
        work->storage.push_back(std::move(*storage));
        auto readback = queue.create_buffer({input.bytes.size(), vk::BufferUsageFlagBits::eTransferDst, BufferMemory::readback});
        if (!readback) return std::unexpected(readback.error());
        work->readbacks.push_back(std::move(*readback));
    }
    try {
        work->first_shader = module(device(), shader);
        Vector<vk::DescriptorSetLayoutBinding> bindings{memory::Allocator<vk::DescriptorSetLayoutBinding>{impl_->resource}};
        for (const auto& binding : shader.bindings)
            bindings.emplace_back(binding.binding, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute);
        vk::DescriptorSetLayoutCreateInfo set_info{};
        set_info.setBindings(bindings);
        work->descriptor_layout = vk::raii::DescriptorSetLayout{logical, set_info};
        vk::PipelineLayoutCreateInfo layout_info{};
        layout_info.setSetLayouts(*work->descriptor_layout);
        const vk::PushConstantRange push{vk::ShaderStageFlagBits::eCompute, 0, static_cast<std::uint32_t>(push_constants.size())};
        if (!push_constants.empty()) layout_info.setPushConstantRanges(push);
        work->layout = vk::raii::PipelineLayout{logical, layout_info};
        const vk::DescriptorPoolSize pool_size{vk::DescriptorType::eStorageBuffer, static_cast<std::uint32_t>(buffers.size())};
        vk::DescriptorPoolCreateInfo pool_info{};
        pool_info.flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet;
        pool_info.maxSets = 1;
        pool_info.setPoolSizes(pool_size);
        work->pool = vk::raii::DescriptorPool{logical, pool_info};
        vk::DescriptorSetAllocateInfo allocation{};
        allocation.setDescriptorPool(*work->pool).setSetLayouts(*work->descriptor_layout);
        auto sets = logical.allocateDescriptorSets(allocation);
        work->descriptor_set = std::move(sets.front());
        for (std::size_t i = 0; i < buffers.size(); ++i) {
            const vk::DescriptorBufferInfo buffer{work->storage[i].handle(), 0, buffers[i].bytes.size()};
            vk::WriteDescriptorSet write{};
            write.setDstSet(*work->descriptor_set).setDstBinding(buffers[i].binding)
                .setDescriptorType(vk::DescriptorType::eStorageBuffer).setBufferInfo(buffer);
            logical.updateDescriptorSets(write, {});
        }
        const vk::PipelineShaderStageCreateInfo stage{{}, vk::ShaderStageFlagBits::eCompute, *work->first_shader, shader.entry.c_str()};
        work->pipeline = vk::raii::Pipeline{logical, nullptr, vk::ComputePipelineCreateInfo{{}, stage, *work->layout}};
        auto batch = queue.begin();
        if (!batch) return std::unexpected(batch.error());
        for (std::size_t i = 0; i < buffers.size(); ++i)
            if (auto status = batch->copy(work->uploads[i], work->storage[i], buffers[i].bytes.size()); !status) return std::unexpected(status.error());
        const auto& command = batch->command_buffer();
        command.bindPipeline(vk::PipelineBindPoint::eCompute, *work->pipeline);
        command.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *work->layout, 0, *work->descriptor_set, {});
        if (!push_constants.empty()) command.pushConstants<std::byte>(*work->layout, vk::ShaderStageFlagBits::eCompute, 0, push_constants);
        command.dispatch(groups[0], groups[1], groups[2]);
        for (std::size_t i = 0; i < buffers.size(); ++i)
            if (auto status = batch->copy(work->storage[i], work->readbacks[i], buffers[i].bytes.size()); !status) return std::unexpected(status.error());
        if (auto status = impl_->execute(std::move(work), std::move(*batch), timeout_ns); !status) return std::unexpected(status.error());
        for (std::size_t i = 0; i < buffers.size(); ++i) {
            auto status = impl_->pending->readbacks[i].read(0, output[i].bytes);
            if (!status) { impl_->pending.reset(); return std::unexpected(status.error()); }
        }
        impl_->pending.reset();
        return output;
    } catch (const vk::SystemError& error) { return std::unexpected(impl_->failure(error)); }
}
} // namespace dk::graphics

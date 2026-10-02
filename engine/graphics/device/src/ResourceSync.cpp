#include "CommandInternal.hpp"

namespace dk::graphics::detail {
Result<void> outside_rendering(const BatchState& batch)
{
    if (auto valid = batch.valid(); !valid) return valid;
    if (batch.rendering) return std::unexpected(Error{ErrorCode::invalid_state, "operation is not allowed inside rendering"});
    return {};
}
bool stage_covers(vk::PipelineStageFlags2 actual, vk::PipelineStageFlags2 requested) noexcept
{
    if (actual & vk::PipelineStageFlagBits2::eAllCommands)
        return !(requested & vk::PipelineStageFlagBits2::eHost) || bool(actual & vk::PipelineStageFlagBits2::eHost);
    if (actual & vk::PipelineStageFlagBits2::eAllGraphics)
        actual |= vk::PipelineStageFlagBits2::eVertexInput | vk::PipelineStageFlagBits2::eVertexShader |
            vk::PipelineStageFlagBits2::eFragmentShader | vk::PipelineStageFlagBits2::eColorAttachmentOutput |
            vk::PipelineStageFlagBits2::eEarlyFragmentTests | vk::PipelineStageFlagBits2::eLateFragmentTests |
            vk::PipelineStageFlagBits2::eIndexInput | vk::PipelineStageFlagBits2::eVertexAttributeInput;
    if (actual & vk::PipelineStageFlagBits2::eAllTransfer)
        actual |= vk::PipelineStageFlagBits2::eCopy | vk::PipelineStageFlagBits2::eBlit | vk::PipelineStageFlagBits2::eClear;
    if (actual & vk::PipelineStageFlagBits2::eVertexInput)
        actual |= vk::PipelineStageFlagBits2::eIndexInput | vk::PipelineStageFlagBits2::eVertexAttributeInput;
    return (actual & requested) == requested;
}
bool access_covers(vk::AccessFlags2 actual, vk::AccessFlags2 requested) noexcept
{
    if (actual & vk::AccessFlagBits2::eMemoryRead) actual |= read_access;
    if (actual & vk::AccessFlagBits2::eMemoryWrite) actual |= write_access;
    if (actual & vk::AccessFlagBits2::eShaderRead) actual |= vk::AccessFlagBits2::eShaderSampledRead | vk::AccessFlagBits2::eShaderStorageRead | vk::AccessFlagBits2::eUniformRead;
    if (actual & vk::AccessFlagBits2::eShaderWrite) actual |= vk::AccessFlagBits2::eShaderStorageWrite;
    return (actual & requested) == requested;
}
Result<ResolvedUse> resolve_use(BatchState& batch, const ResourceUse& input)
{
    if (bool(input.buffer) == bool(input.image)) return std::unexpected(Error{ErrorCode::invalid_argument, "use requires exactly one buffer or image"});
    auto resource = input.buffer ? ObjectAccess::state(*input.buffer) : ObjectAccess::state(*input.image);
    if (auto valid = batch.check(resource); !valid) return std::unexpected(valid.error());
    const auto& state = input.state;
    constexpr auto allowed_stages = vk::PipelineStageFlagBits2::eTopOfPipe | vk::PipelineStageFlagBits2::eBottomOfPipe |
        vk::PipelineStageFlagBits2::eAllCommands | vk::PipelineStageFlagBits2::eAllGraphics | vk::PipelineStageFlagBits2::eAllTransfer |
        vk::PipelineStageFlagBits2::eCopy | vk::PipelineStageFlagBits2::eBlit | vk::PipelineStageFlagBits2::eClear |
        vk::PipelineStageFlagBits2::eVertexInput | vk::PipelineStageFlagBits2::eIndexInput | vk::PipelineStageFlagBits2::eVertexAttributeInput |
        vk::PipelineStageFlagBits2::eVertexShader | vk::PipelineStageFlagBits2::eFragmentShader | vk::PipelineStageFlagBits2::eComputeShader |
        vk::PipelineStageFlagBits2::eEarlyFragmentTests | vk::PipelineStageFlagBits2::eLateFragmentTests |
        vk::PipelineStageFlagBits2::eColorAttachmentOutput | vk::PipelineStageFlagBits2::eHost;
    if (!state.stages || !state.access || (state.stages & ~allowed_stages) || (state.access & ~(read_access | write_access)) ||
        (input.full_overwrite && !(state.access & write_access)))
        return std::unexpected(Error{ErrorCode::invalid_argument, "unsupported resource access/stages or overwrite declaration"});
    const auto has_stage = [&](vk::PipelineStageFlags2 choices) {
        constexpr auto graphics = vk::PipelineStageFlagBits2::eVertexShader | vk::PipelineStageFlagBits2::eFragmentShader |
            vk::PipelineStageFlagBits2::eVertexInput | vk::PipelineStageFlagBits2::eIndexInput | vk::PipelineStageFlagBits2::eVertexAttributeInput |
            vk::PipelineStageFlagBits2::eEarlyFragmentTests | vk::PipelineStageFlagBits2::eLateFragmentTests | vk::PipelineStageFlagBits2::eColorAttachmentOutput;
        return bool(state.stages & choices) || bool(state.stages & vk::PipelineStageFlagBits2::eAllCommands) ||
            (bool(state.stages & vk::PipelineStageFlagBits2::eAllGraphics) && bool(choices & graphics)) ||
            (bool(state.stages & vk::PipelineStageFlagBits2::eVertexInput) && bool(choices & (vk::PipelineStageFlagBits2::eIndexInput | vk::PipelineStageFlagBits2::eVertexAttributeInput)));
    };
    if ((state.access & (vk::AccessFlagBits2::eTransferRead | vk::AccessFlagBits2::eTransferWrite)) &&
        !has_stage(vk::PipelineStageFlagBits2::eAllTransfer | vk::PipelineStageFlagBits2::eCopy | vk::PipelineStageFlagBits2::eBlit | vk::PipelineStageFlagBits2::eClear))
        return std::unexpected(Error{ErrorCode::invalid_argument, "transfer access requires transfer stages"});
    if ((state.access & (vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite | vk::AccessFlagBits2::eShaderSampledRead |
        vk::AccessFlagBits2::eShaderStorageRead | vk::AccessFlagBits2::eShaderStorageWrite | vk::AccessFlagBits2::eUniformRead)) &&
        !has_stage(vk::PipelineStageFlagBits2::eVertexShader | vk::PipelineStageFlagBits2::eFragmentShader | vk::PipelineStageFlagBits2::eComputeShader))
        return std::unexpected(Error{ErrorCode::invalid_argument, "shader access requires shader stages"});
    const auto stage_matches = [&](vk::AccessFlags2 access, vk::PipelineStageFlags2 stages) { return !(state.access & access) || has_stage(stages); };
    if (((state.access & (vk::AccessFlagBits2::eHostRead | vk::AccessFlagBits2::eHostWrite)) && !(state.stages & vk::PipelineStageFlagBits2::eHost)) ||
        !stage_matches(vk::AccessFlagBits2::eIndexRead, vk::PipelineStageFlagBits2::eIndexInput) ||
        !stage_matches(vk::AccessFlagBits2::eVertexAttributeRead, vk::PipelineStageFlagBits2::eVertexAttributeInput) ||
        !stage_matches(vk::AccessFlagBits2::eColorAttachmentRead | vk::AccessFlagBits2::eColorAttachmentWrite, vk::PipelineStageFlagBits2::eColorAttachmentOutput) ||
        !stage_matches(vk::AccessFlagBits2::eDepthStencilAttachmentRead | vk::AccessFlagBits2::eDepthStencilAttachmentWrite, vk::PipelineStageFlagBits2::eEarlyFragmentTests | vk::PipelineStageFlagBits2::eLateFragmentTests))
        return std::unexpected(Error{ErrorCode::invalid_argument, "access and pipeline stages are incompatible"});
    ResolvedUse use{resource, state, input.offset, input.size, input.range, input.full_overwrite};
    if (input.buffer) {
        const auto& desc = resource->buffer_desc;
        if (((state.access & vk::AccessFlagBits2::eHostRead) && desc.memory != BufferMemory::readback) ||
            ((state.access & vk::AccessFlagBits2::eHostWrite) && desc.memory != BufferMemory::upload))
            return std::unexpected(Error{ErrorCode::invalid_argument, "host access does not match buffer memory role"});
        if (state.access & (vk::AccessFlagBits2::eShaderSampledRead | vk::AccessFlagBits2::eColorAttachmentRead | vk::AccessFlagBits2::eColorAttachmentWrite |
            vk::AccessFlagBits2::eDepthStencilAttachmentRead | vk::AccessFlagBits2::eDepthStencilAttachmentWrite))
            return std::unexpected(Error{ErrorCode::invalid_argument, "image access requested for buffer"});
        if (state.layout != vk::ImageLayout::eUndefined || input.offset > desc.size)
            return std::unexpected(Error{ErrorCode::invalid_argument, "buffer use layout/offset invalid"});
        use.size = input.size == VK_WHOLE_SIZE ? desc.size-input.offset : input.size;
        if (!use.size || !valid_range(desc.size, use.offset, use.size)) return std::unexpected(Error{ErrorCode::invalid_argument, "buffer use range invalid"});
        const auto usage_for = [&](vk::AccessFlags2 access, vk::BufferUsageFlags usage) { return !(state.access & access) || bool(desc.usage & usage); };
        if (!usage_for(vk::AccessFlagBits2::eTransferRead, vk::BufferUsageFlagBits::eTransferSrc) ||
            !usage_for(vk::AccessFlagBits2::eTransferWrite, vk::BufferUsageFlagBits::eTransferDst) ||
            !usage_for(vk::AccessFlagBits2::eUniformRead, vk::BufferUsageFlagBits::eUniformBuffer) ||
            !usage_for(vk::AccessFlagBits2::eVertexAttributeRead, vk::BufferUsageFlagBits::eVertexBuffer) ||
            !usage_for(vk::AccessFlagBits2::eIndexRead, vk::BufferUsageFlagBits::eIndexBuffer) ||
            !usage_for(vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite | vk::AccessFlagBits2::eShaderStorageRead | vk::AccessFlagBits2::eShaderStorageWrite, vk::BufferUsageFlagBits::eStorageBuffer))
            return std::unexpected(Error{ErrorCode::invalid_argument, "buffer access does not match usage"});
    } else {
        if (state.access & (vk::AccessFlagBits2::eHostRead | vk::AccessFlagBits2::eHostWrite | vk::AccessFlagBits2::eIndexRead |
            vk::AccessFlagBits2::eVertexAttributeRead | vk::AccessFlagBits2::eUniformRead))
            return std::unexpected(Error{ErrorCode::invalid_argument, "buffer access requested for image"});
        if (auto valid = validate_subresources(resource->image_desc, use.range); !valid) return std::unexpected(valid.error());
        if (auto valid = validate_layout(state.layout, resource->image_desc.usage); !valid) return std::unexpected(valid.error());
        vk::AccessFlags2 layout_access = read_access | write_access;
        switch (state.layout) {
        case vk::ImageLayout::eTransferSrcOptimal: layout_access = vk::AccessFlagBits2::eTransferRead; break;
        case vk::ImageLayout::eTransferDstOptimal: layout_access = vk::AccessFlagBits2::eTransferWrite; break;
        case vk::ImageLayout::eShaderReadOnlyOptimal: layout_access = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderSampledRead; break;
        case vk::ImageLayout::eColorAttachmentOptimal: layout_access = vk::AccessFlagBits2::eColorAttachmentRead | vk::AccessFlagBits2::eColorAttachmentWrite; break;
        case vk::ImageLayout::eDepthStencilAttachmentOptimal: layout_access = vk::AccessFlagBits2::eDepthStencilAttachmentRead | vk::AccessFlagBits2::eDepthStencilAttachmentWrite; break;
        default: break;
        }
        if (state.access & ~layout_access) return std::unexpected(Error{ErrorCode::invalid_argument, "image access incompatible with layout"});
        const auto usage_for = [&](vk::AccessFlags2 access, vk::ImageUsageFlags usage) { return !(state.access & access) || bool(resource->image_desc.usage & usage); };
        if (!usage_for(vk::AccessFlagBits2::eTransferRead, vk::ImageUsageFlagBits::eTransferSrc) ||
            !usage_for(vk::AccessFlagBits2::eTransferWrite, vk::ImageUsageFlagBits::eTransferDst) ||
            !usage_for(vk::AccessFlagBits2::eShaderSampledRead, vk::ImageUsageFlagBits::eSampled) ||
            !usage_for(vk::AccessFlagBits2::eShaderWrite | vk::AccessFlagBits2::eShaderStorageRead | vk::AccessFlagBits2::eShaderStorageWrite, vk::ImageUsageFlagBits::eStorage) ||
            !usage_for(vk::AccessFlagBits2::eColorAttachmentRead | vk::AccessFlagBits2::eColorAttachmentWrite, vk::ImageUsageFlagBits::eColorAttachment) ||
            !usage_for(vk::AccessFlagBits2::eDepthStencilAttachmentRead | vk::AccessFlagBits2::eDepthStencilAttachmentWrite, vk::ImageUsageFlagBits::eDepthStencilAttachment))
            return std::unexpected(Error{ErrorCode::invalid_argument, "image access does not match usage"});
    }
    return use;
}
namespace {
template<class F> void each_subresource(const ResourceState& resource, const vk::ImageSubresourceRange& range, F action)
{
    if (resource.buffer) { action(std::size_t{0}, 0u, 0u); return; }
    for (std::uint32_t layer = range.baseArrayLayer; layer < range.baseArrayLayer+range.layerCount; ++layer)
        for (std::uint32_t mip = range.baseMipLevel; mip < range.baseMipLevel+range.levelCount; ++mip)
            action(static_cast<std::size_t>(layer)*resource.image_desc.mip_levels+mip, mip, layer);
}
}
Result<void> prepare_resolved(BatchState& batch, std::span<const ResolvedUse> uses, std::span<const AccessState> before)
{
    if (auto valid = outside_rendering(batch); !valid) return valid;
    if (!before.empty() && before.size() != uses.size()) return std::unexpected(Error{ErrorCode::invalid_argument, "barrier state count mismatch"});
    RetainRollback rollback{batch};
    Vector<vk::BufferMemoryBarrier2> buffers{memory::Allocator<vk::BufferMemoryBarrier2>{batch.queue->resource}};
    Vector<vk::ImageMemoryBarrier2> images{memory::Allocator<vk::ImageMemoryBarrier2>{batch.queue->resource}};
    for (std::size_t i = 0; i < uses.size(); ++i) {
        const auto& use = uses[i];
        for (std::size_t j = 0; j < i; ++j) if (uses[j].resource == use.resource) {
            const auto& a = uses[j].range;
            const auto& b = use.range;
            if (use.resource->buffer || (a.baseMipLevel < b.baseMipLevel+b.levelCount && b.baseMipLevel < a.baseMipLevel+a.levelCount &&
                a.baseArrayLayer < b.baseArrayLayer+b.layerCount && b.baseArrayLayer < a.baseArrayLayer+a.layerCount))
                return std::unexpected(Error{ErrorCode::invalid_argument, "overlapping resource preparations must be merged"});
        }
        if (auto retained = batch.retain(use.resource); !retained) return retained;
        auto* local = batch.find(use.resource);
        bool mismatch = false;
        each_subresource(*use.resource, use.range, [&](std::size_t index, std::uint32_t mip, std::uint32_t layer) {
            const auto& prior = local->states[index];
            if (!before.empty() && before[i] != prior) { mismatch = true; return; }
            if (use.resource->buffer) {
                vk::BufferMemoryBarrier2 barrier{};
                barrier.setSrcStageMask(prior.stages).setSrcAccessMask(prior.access).setDstStageMask(use.state.stages).setDstAccessMask(use.state.access)
                    .setSrcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED).setDstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
                    .setBuffer(use.resource->buffer).setOffset(0).setSize(VK_WHOLE_SIZE);
                buffers.push_back(barrier);
            } else {
                vk::ImageMemoryBarrier2 barrier{};
                barrier.setSrcStageMask(prior.stages).setSrcAccessMask(prior.access).setDstStageMask(use.state.stages).setDstAccessMask(use.state.access)
                    .setOldLayout(prior.layout).setNewLayout(use.state.layout).setImage(use.resource->image)
                    .setSrcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED).setDstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
                    .setSubresourceRange(vk::ImageSubresourceRange{use.range.aspectMask, mip, 1, layer, 1});
                images.push_back(barrier);
            }
        });
        if (mismatch) return std::unexpected(Error{ErrorCode::conflict, "explicit barrier before state disagrees with tracked state"});
    }
    vk::DependencyInfo info{};
    info.setBufferMemoryBarriers(buffers).setImageMemoryBarriers(images);
    if (!buffers.empty() || !images.empty()) batch.command().pipelineBarrier2(info);
    for (const auto& use : uses) {
        auto* local = batch.find(use.resource);
        each_subresource(*use.resource, use.range, [&](std::size_t index, std::uint32_t, std::uint32_t) {
            auto& state = local->states[index];
            const bool keep_reads = !(state.access & write_access) && !(use.state.access & write_access) && state.layout == use.state.layout;
            const auto stages = keep_reads ? state.stages | use.state.stages : use.state.stages;
            const auto access = keep_reads ? state.access | use.state.access : use.state.access;
            state = {stages, access, use.state.layout, state.initialized};
            local->prepared[index] = static_cast<std::uint8_t>(1u | (use.full_overwrite ? 2u : 0u));
        });
    }
    rollback.committed = true;
    return {};
}
Result<void> require_use(BatchState& batch, const std::shared_ptr<ResourceState>& resource, vk::PipelineStageFlags2 stages,
    vk::AccessFlags2 access, const vk::ImageSubresourceRange* range, vk::ImageLayout layout, bool require_content)
{
    if (auto valid = batch.check(resource); !valid) return valid;
    if (resource->image && !range) return std::unexpected(Error{ErrorCode::invalid_argument, "image access requires a subresource range"});
    const auto* use = batch.find(resource);
    if (!use) return std::unexpected(Error{ErrorCode::invalid_state, "resource must be prepared before the command"});
    bool valid = true;
    each_subresource(*resource, range ? *range : vk::ImageSubresourceRange{}, [&](std::size_t index, std::uint32_t, std::uint32_t) {
        const auto& state = use->states[index];
        if (!(use->prepared[index] & 1u) || !stage_covers(state.stages, stages) || !access_covers(state.access, access) ||
            (resource->image && (state.layout != layout || (require_content && (access & read_access) && !state.initialized)))) valid = false;
    });
    if (!valid) return std::unexpected(Error{ErrorCode::invalid_state, "resource access requires matching prepare/barrier and initialized image content"});
    return {};
}
void consume_use(BatchState& batch, const std::shared_ptr<ResourceState>& resource, const vk::ImageSubresourceRange* range, bool initializes) noexcept
{
    auto* use = batch.find(resource);
    each_subresource(*resource, range ? *range : vk::ImageSubresourceRange{}, [&](std::size_t index, std::uint32_t, std::uint32_t) {
        if (use->states[index].access & write_access) {
            use->states[index].initialized = use->states[index].initialized || initializes || (use->prepared[index] & 2u);
            use->prepared[index] = 0;
        }
    });
}
Result<void> retain_object(BatchState& batch, const std::shared_ptr<ObjectState>& object)
{
    if (auto valid = batch.valid(); !valid) return valid;
    if (!object || object->owner != batch.queue->owner) return std::unexpected(Error{ErrorCode::invalid_argument, "GPU object is empty or foreign"});
    if (std::find(batch.objects.begin(), batch.objects.end(), object) == batch.objects.end()) batch.objects.push_back(object);
    return {};
}
void invalidate_encoder(BatchState& batch) noexcept { ++batch.generation; batch.encoding.reset(); }
} // namespace dk::graphics::detail

namespace dk::graphics {
ResourceUse buffer_use(const Buffer& buffer, vk::PipelineStageFlags2 stages, vk::AccessFlags2 access, vk::DeviceSize offset, vk::DeviceSize size)
{ ResourceUse use{}; use.buffer = &buffer; use.state = {stages, access}; use.offset = offset; use.size = size; return use; }
ResourceUse image_use(const Image& image, vk::PipelineStageFlags2 stages, vk::AccessFlags2 access, vk::ImageLayout layout, vk::ImageSubresourceRange range)
{ ResourceUse use{}; use.image = &image; use.state = {stages, access, layout}; use.range = range; return use; }
Result<void> CommandBatch::prepare(std::span<const ResourceUse> uses)
{
    if (!state_) return std::unexpected(Error{ErrorCode::invalid_state, "empty batch"});
    Vector<detail::ResolvedUse> resolved{memory::Allocator<detail::ResolvedUse>{state_->queue->resource}};
    resolved.reserve(uses.size());
    for (const auto& use : uses) {
        auto value = detail::resolve_use(*state_, use);
        if (!value) return std::unexpected(value.error());
        resolved.push_back(std::move(*value));
    }
    return detail::prepare_resolved(*state_, resolved);
}
Result<void> CommandBatch::barrier(std::span<const ResourceBarrier> barriers)
{
    if (!state_) return std::unexpected(Error{ErrorCode::invalid_state, "empty batch"});
    Vector<detail::ResolvedUse> resolved{memory::Allocator<detail::ResolvedUse>{state_->queue->resource}};
    Vector<AccessState> before{memory::Allocator<AccessState>{state_->queue->resource}};
    for (const auto& barrier : barriers) {
        auto value = detail::resolve_use(*state_, barrier.resource);
        if (!value) return std::unexpected(value.error());
        resolved.push_back(std::move(*value));
        before.push_back(barrier.before);
    }
    return detail::prepare_resolved(*state_, resolved, before);
}
} // namespace dk::graphics

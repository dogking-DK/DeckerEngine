#include "CommandInternal.hpp"
#include <dk/graphics/ResourceValidation.hpp>

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
    const AccessDescription description{input.state, input.offset, input.size, input.range, input.full_overwrite};
    auto valid = input.buffer ? validate_buffer_access(resource->buffer_desc, description)
                              : validate_image_access(resource->image_desc, description);
    if (!valid) return std::unexpected(valid.error());
    return ResolvedUse{resource, valid->state, valid->offset, valid->size, valid->range, valid->full_overwrite};
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
            const bool full = use.full_overwrite && (!use.resource->buffer ||
                (use.offset == 0 && use.size == use.resource->buffer_desc.size));
            local->prepared[index] = static_cast<std::uint8_t>(1u | (full ? 2u : 0u));
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
Result<AccessState> CommandBatch::state(const Buffer& buffer) const
{
    if (!state_) return std::unexpected(Error{ErrorCode::invalid_state, "empty batch"});
    if (auto valid = state_->check(buffer.state_); !valid) return std::unexpected(valid.error());
    const auto* local = state_->find(buffer.state_);
    return local ? local->states.front() : buffer.state_->states.front();
}
Result<AccessState> CommandBatch::state(const Image& image, std::uint32_t mip, std::uint32_t layer) const
{
    if (!state_) return std::unexpected(Error{ErrorCode::invalid_state, "empty batch"});
    if (auto valid = state_->check(image.state_); !valid) return std::unexpected(valid.error());
    const auto desc = image.description();
    if (mip >= desc.mip_levels || layer >= desc.array_layers)
        return std::unexpected(Error{ErrorCode::invalid_argument, "image state subresource is out of range"});
    const auto* local = state_->find(image.state_);
    const auto index = static_cast<std::size_t>(layer) * desc.mip_levels + mip;
    return local ? local->states[index] : image.state_->states[index];
}
Result<void> CommandBatch::finish_pass()
{
    if (!state_) return std::unexpected(Error{ErrorCode::invalid_state, "empty batch"});
    if (auto valid = detail::outside_rendering(*state_); !valid) return valid;
    for (const auto& use : state_->uses) for (std::size_t i = 0; i < use.states.size(); ++i)
        if ((use.prepared[i] & 1u) && (use.states[i].access & detail::write_access))
            return std::unexpected(Error{ErrorCode::invalid_state, "pass has an unconsumed declared write"});
    for (auto& use : state_->uses) std::fill(use.prepared.begin(), use.prepared.end(), std::uint8_t{0});
    detail::invalidate_encoder(*state_);
    return {};
}
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

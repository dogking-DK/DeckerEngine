#include "CommandInternal.hpp"

namespace dk::graphics::detail {
Result<CopyFootprint> copy_footprint(const ImageDesc& image, const ImageCopyRegion& region)
{
    if (auto bytes = image_bytes(image); !bytes) return std::unexpected(bytes.error());
    if (region.mip >= image.mip_levels || region.layer >= image.array_layers || !region.width || !region.height || region.buffer_offset % 4)
        return std::unexpected(Error{ErrorCode::invalid_argument, "invalid copy mip/layer/extent/alignment"});
    const auto width = std::max(1u, image.width >> region.mip), height = std::max(1u, image.height >> region.mip);
    const auto row = region.row_length ? region.row_length : region.width;
    if (!valid_range(width, region.x, region.width) || !valid_range(height, region.y, region.height) || row < region.width ||
        (region.image_height && region.image_height < region.height) || row > static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max() / 4))
        return std::unexpected(Error{ErrorCode::invalid_argument, "copy extent or row pitch is out of range"});
    const auto texels = vk::DeviceSize{row} * (region.height - 1) + region.width;
    if (texels > std::numeric_limits<vk::DeviceSize>::max() / 4 || region.buffer_offset > std::numeric_limits<vk::DeviceSize>::max() - texels * 4)
        return std::unexpected(Error{ErrorCode::invalid_argument, "copy footprint overflow"});
    CopyFootprint result{};
    result.native = vk::BufferImageCopy{region.buffer_offset, region.row_length, region.image_height,
        vk::ImageSubresourceLayers{vk::ImageAspectFlagBits::eColor, region.mip, region.layer, 1},
        vk::Offset3D{static_cast<std::int32_t>(region.x), static_cast<std::int32_t>(region.y), 0}, vk::Extent3D{region.width, region.height, 1}};
    result.range = vk::ImageSubresourceRange{vk::ImageAspectFlagBits::eColor, region.mip, 1, region.layer, 1};
    result.bytes = texels * 4;
    result.full = region.x == 0 && region.y == 0 && region.width == width && region.height == height;
    return result;
}
} // namespace dk::graphics::detail

namespace dk::graphics {
namespace {
Result<void> ready(const std::shared_ptr<detail::BatchState>& state)
{ return state ? detail::outside_rendering(*state) : std::unexpected(Error{ErrorCode::invalid_state, "empty command batch"}); }
vk::ImageLayout local_layout(detail::BatchState& batch, const std::shared_ptr<detail::ResourceState>& image, const vk::ImageSubresourceRange& range)
{
    const auto* use = batch.find(image);
    return use ? use->states[static_cast<std::size_t>(range.baseArrayLayer) * image->image_desc.mip_levels + range.baseMipLevel].layout : vk::ImageLayout::eUndefined;
}
}
Result<void> CommandBatch::copy_buffer(const Buffer& source, const Buffer& destination, vk::DeviceSize size,
    vk::DeviceSize source_offset, vk::DeviceSize destination_offset)
{
    if (auto valid = ready(state_); !valid) return valid;
    if (auto valid = detail::validate_copy(source.size(), destination.size(), size, source_offset, destination_offset, source.handle() == destination.handle()); !valid) return valid;
    if (auto valid = detail::require_use(*state_, source.state_, vk::PipelineStageFlagBits2::eCopy, vk::AccessFlagBits2::eTransferRead); !valid) return valid;
    if (auto valid = detail::require_use(*state_, destination.state_, vk::PipelineStageFlagBits2::eCopy, vk::AccessFlagBits2::eTransferWrite); !valid) return valid;
    state_->command().copyBuffer(source.handle(), destination.handle(), vk::BufferCopy{source_offset, destination_offset, size});
    detail::consume_use(*state_, destination.state_, nullptr, destination_offset == 0 && size == destination.size());
    return {};
}
Result<void> CommandBatch::copy_to_image(const Buffer& source, const Image& destination, const ImageCopyRegion& region)
{
    if (auto valid = ready(state_); !valid) return valid;
    if (auto valid = state_->check(destination.state_); !valid) return valid;
    auto footprint = detail::copy_footprint(destination.description(), region);
    if (!footprint) return std::unexpected(footprint.error());
    if (!detail::valid_range(source.size(), region.buffer_offset, footprint->bytes)) return std::unexpected(Error{ErrorCode::invalid_argument, "upload buffer too small"});
    const auto layout = local_layout(*state_, destination.state_, footprint->range);
    if (layout != vk::ImageLayout::eTransferDstOptimal && layout != vk::ImageLayout::eGeneral)
        return std::unexpected(Error{ErrorCode::invalid_state, "copy destination layout is not prepared"});
    if (auto valid = detail::require_use(*state_, source.state_, vk::PipelineStageFlagBits2::eCopy, vk::AccessFlagBits2::eTransferRead); !valid) return valid;
    if (auto valid = detail::require_use(*state_, destination.state_, vk::PipelineStageFlagBits2::eCopy, vk::AccessFlagBits2::eTransferWrite, &footprint->range, layout, false); !valid) return valid;
    state_->command().copyBufferToImage(source.handle(), destination.handle(), layout, footprint->native);
    detail::consume_use(*state_, destination.state_, &footprint->range, footprint->full);
    return {};
}
Result<void> CommandBatch::copy_to_buffer(const Image& source, const Buffer& destination, const ImageCopyRegion& region)
{
    if (auto valid = ready(state_); !valid) return valid;
    if (auto valid = state_->check(source.state_); !valid) return valid;
    auto footprint = detail::copy_footprint(source.description(), region);
    if (!footprint) return std::unexpected(footprint.error());
    if (!detail::valid_range(destination.size(), region.buffer_offset, footprint->bytes)) return std::unexpected(Error{ErrorCode::invalid_argument, "readback buffer too small"});
    const auto layout = local_layout(*state_, source.state_, footprint->range);
    if (layout != vk::ImageLayout::eTransferSrcOptimal && layout != vk::ImageLayout::eGeneral)
        return std::unexpected(Error{ErrorCode::invalid_state, "copy source layout is not prepared"});
    if (auto valid = detail::require_use(*state_, source.state_, vk::PipelineStageFlagBits2::eCopy, vk::AccessFlagBits2::eTransferRead, &footprint->range, layout); !valid) return valid;
    if (auto valid = detail::require_use(*state_, destination.state_, vk::PipelineStageFlagBits2::eCopy, vk::AccessFlagBits2::eTransferWrite); !valid) return valid;
    state_->command().copyImageToBuffer(source.handle(), layout, destination.handle(), footprint->native);
    detail::consume_use(*state_, destination.state_, nullptr, region.buffer_offset == 0 && footprint->bytes == destination.size());
    return {};
}
Result<void> CommandBatch::fill(const Buffer& buffer, std::uint32_t value)
{
    if (auto valid = ready(state_); !valid) return valid;
    if (!buffer.size() || buffer.size() % 4) return std::unexpected(Error{ErrorCode::invalid_argument, "fill requires a nonempty four-byte sized buffer"});
    if (auto valid = detail::require_use(*state_, buffer.state_, vk::PipelineStageFlagBits2::eClear, vk::AccessFlagBits2::eTransferWrite); !valid) return valid;
    state_->command().fillBuffer(buffer.handle(), 0, buffer.size(), value);
    detail::consume_use(*state_, buffer.state_, nullptr, true);
    return {};
}
Result<void> CommandBatch::clear(const Image& image, const vk::ClearColorValue& color, const vk::ImageSubresourceRange& range)
{
    if (auto valid = ready(state_); !valid) return valid;
    if (auto valid = state_->check(image.state_); !valid) return valid;
    if (auto valid = detail::validate_subresources(image.description(), range); !valid) return valid;
    if (range.aspectMask != vk::ImageAspectFlagBits::eColor) return std::unexpected(Error{ErrorCode::not_supported, "clear supports color images"});
    const auto layout = local_layout(*state_, image.state_, range);
    if (layout != vk::ImageLayout::eTransferDstOptimal && layout != vk::ImageLayout::eGeneral)
        return std::unexpected(Error{ErrorCode::invalid_state, "clear image layout is not prepared"});
    if (auto valid = detail::require_use(*state_, image.state_, vk::PipelineStageFlagBits2::eClear, vk::AccessFlagBits2::eTransferWrite, &range, layout, false); !valid) return valid;
    state_->command().clearColorImage(image.handle(), layout, color, range);
    detail::consume_use(*state_, image.state_, &range, true);
    return {};
}
Result<void> CommandBatch::unsafe_record(std::span<const ResourceUse> before, std::span<const ResourceUse> after, NativeRecorder recorder, void* user_data)
{
    if (auto valid = ready(state_); !valid) return valid;
    if (!recorder || before.size() != after.size()) return std::unexpected(Error{ErrorCode::invalid_argument, "native recorder requires paired before/after declarations"});
    Vector<detail::ResolvedUse> final{memory::Allocator<detail::ResolvedUse>{state_->queue->resource}};
    for (std::size_t i = 0; i < before.size(); ++i) {
        auto resolved = detail::resolve_use(*state_, after[i]);
        if (!resolved) return std::unexpected(resolved.error());
        if (before[i].buffer != after[i].buffer || before[i].image != after[i].image || before[i].range != after[i].range ||
            before[i].offset != after[i].offset || before[i].size != after[i].size)
            return std::unexpected(Error{ErrorCode::invalid_argument, "native recorder must declare the same resources/ranges before and after"});
        final.push_back(std::move(*resolved));
    }
    if (auto valid = prepare(before); !valid) return valid;
    detail::invalidate_encoder(*state_);
    try { recorder(state_->command(), user_data); }
    catch (...) { state_->invalid = true; throw; }
    for (const auto& use : final) {
        auto* local = state_->find(use.resource);
        const auto publish = [&](std::size_t index) {
            const bool initialized = local->states[index].initialized || use.full_overwrite;
            local->states[index] = use.state;
            local->states[index].initialized = initialized;
            local->prepared[index] = 0;
        };
        if (use.resource->buffer) publish(0);
        else for (std::uint32_t layer = use.range.baseArrayLayer; layer < use.range.baseArrayLayer + use.range.layerCount; ++layer)
            for (std::uint32_t mip = use.range.baseMipLevel; mip < use.range.baseMipLevel + use.range.levelCount; ++mip)
                publish(static_cast<std::size_t>(layer) * use.resource->image_desc.mip_levels + mip);
    }
    return {};
}
} // namespace dk::graphics

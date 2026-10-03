#pragma once
#include <dk/graphics/Resources.hpp>
#include <bit>
#include <algorithm>

namespace dk::graphics::detail {
inline Result<void> validate_buffer(const BufferDesc& desc)
{
    constexpr auto allowed = vk::BufferUsageFlagBits::eTransferSrc | vk::BufferUsageFlagBits::eTransferDst |
        vk::BufferUsageFlagBits::eUniformTexelBuffer | vk::BufferUsageFlagBits::eStorageTexelBuffer |
        vk::BufferUsageFlagBits::eUniformBuffer | vk::BufferUsageFlagBits::eStorageBuffer |
        vk::BufferUsageFlagBits::eIndexBuffer | vk::BufferUsageFlagBits::eVertexBuffer | vk::BufferUsageFlagBits::eIndirectBuffer;
    if (!desc.size || !desc.usage || (static_cast<VkBufferUsageFlags>(desc.usage) & ~static_cast<VkBufferUsageFlags>(allowed)))
        return std::unexpected(Error{ErrorCode::invalid_argument, "buffer requires nonzero size and supported usage"});
    if (desc.memory != BufferMemory::device && desc.memory != BufferMemory::upload && desc.memory != BufferMemory::readback)
        return std::unexpected(Error{ErrorCode::invalid_argument, "invalid buffer memory role"});
    return {};
}
inline std::uint32_t color_texel_bytes(vk::Format format) noexcept
{
    switch (format) {
    case vk::Format::eR8G8B8A8Unorm: case vk::Format::eR8G8B8A8Srgb: case vk::Format::eB8G8R8A8Unorm: case vk::Format::eB8G8R8A8Srgb:
    case vk::Format::eR32Uint: case vk::Format::eR32Sfloat: return 4;
    case vk::Format::eR32G32B32A32Sfloat: return 16;
    default: return 0;
    }
}
inline Result<vk::DeviceSize> image_bytes(const ImageDesc& desc)
{
    constexpr auto allowed = vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst |
        vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eStorage |
        vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eInputAttachment;
    if (!desc.width || !desc.height || !desc.usage || (static_cast<VkImageUsageFlags>(desc.usage) & ~static_cast<VkImageUsageFlags>(allowed)))
        return std::unexpected(Error{ErrorCode::invalid_argument, "image requires nonzero extent and supported color usage"});
    const auto texel_bytes = detail::color_texel_bytes(desc.format);
    if (!texel_bytes) return std::unexpected(Error{ErrorCode::not_supported, "unsupported color image format"});
    const auto pixels = vk::DeviceSize{desc.width} * desc.height;
    if (pixels > std::numeric_limits<vk::DeviceSize>::max() / texel_bytes)
        return std::unexpected(Error{ErrorCode::invalid_argument, "image byte count overflow"});
    return pixels * texel_bytes;
}
inline bool valid_range(vk::DeviceSize capacity, vk::DeviceSize offset, vk::DeviceSize size) noexcept
{ return offset <= capacity && size <= capacity - offset; }
inline Result<void> validate_image(const ImageDesc& desc)
{
    if (!desc.width || !desc.height || !desc.mip_levels || !desc.array_layers ||
        desc.mip_levels > static_cast<std::uint32_t>(std::bit_width(std::max(desc.width, desc.height))))
        return std::unexpected(Error{ErrorCode::invalid_argument, "invalid image mip/layer/extent"});
    if (desc.format == vk::Format::eD32Sfloat) {
        constexpr auto allowed = vk::ImageUsageFlagBits::eDepthStencilAttachment | vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst;
        if (!desc.usage || (static_cast<VkImageUsageFlags>(desc.usage) & ~static_cast<VkImageUsageFlags>(allowed)))
            return std::unexpected(Error{ErrorCode::not_supported, "depth images support attachment, sampling and transfer clear"});
        return {};
    }
    if (auto bytes = image_bytes(desc); !bytes) return std::unexpected(bytes.error());
    return {};
}
inline Result<void> validate_subresources(const ImageDesc& desc, const vk::ImageSubresourceRange& range)
{
    const auto aspect = desc.format == vk::Format::eD32Sfloat ? vk::ImageAspectFlagBits::eDepth : vk::ImageAspectFlagBits::eColor;
    if (range.aspectMask != aspect || !range.levelCount || !range.layerCount ||
        !valid_range(desc.mip_levels, range.baseMipLevel, range.levelCount) ||
        !valid_range(desc.array_layers, range.baseArrayLayer, range.layerCount))
        return std::unexpected(Error{ErrorCode::invalid_argument, "invalid image aspect/mip/layer range"});
    return {};
}
inline Result<void> validate_copy(vk::DeviceSize source_size, vk::DeviceSize destination_size,
    vk::DeviceSize size, vk::DeviceSize source_offset, vk::DeviceSize destination_offset, bool same)
{
    if (!size || (size % 4) || (source_offset % 4) || (destination_offset % 4) ||
        !valid_range(source_size, source_offset, size) || !valid_range(destination_size, destination_offset, size))
        return std::unexpected(Error{ErrorCode::invalid_argument, "buffer copy requires nonempty aligned in-range regions"});
    if (same && source_offset < destination_offset + size && destination_offset < source_offset + size)
        return std::unexpected(Error{ErrorCode::invalid_argument, "same-buffer copy regions overlap"});
    return {};
}
inline Result<void> validate_layout(vk::ImageLayout layout, vk::ImageUsageFlags usage)
{
    bool supported = false;
    switch (layout) {
    case vk::ImageLayout::eTransferSrcOptimal: supported = bool(usage & vk::ImageUsageFlagBits::eTransferSrc); break;
    case vk::ImageLayout::eTransferDstOptimal: supported = bool(usage & vk::ImageUsageFlagBits::eTransferDst); break;
    case vk::ImageLayout::eShaderReadOnlyOptimal: supported = bool(usage & (vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eInputAttachment)); break;
    case vk::ImageLayout::eGeneral: supported = true; break;
    case vk::ImageLayout::eColorAttachmentOptimal: supported = bool(usage & vk::ImageUsageFlagBits::eColorAttachment); break;
    case vk::ImageLayout::eDepthStencilAttachmentOptimal: supported = bool(usage & vk::ImageUsageFlagBits::eDepthStencilAttachment); break;
    default: break;
    }
    if (!supported) return std::unexpected(Error{ErrorCode::invalid_argument, "unsupported image layout or missing usage"});
    return {};
}
} // namespace dk::graphics::detail

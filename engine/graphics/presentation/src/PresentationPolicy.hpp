#pragma once
#include <dk/graphics/Presentation.hpp>
#include <algorithm>

namespace dk::graphics::detail {
struct SwapchainChoice {
    vk::SurfaceFormatKHR format;
    vk::Extent2D extent;
    std::uint32_t images = 0;
    vk::CompositeAlphaFlagBitsKHR alpha = vk::CompositeAlphaFlagBitsKHR::eOpaque;
    vk::ImageUsageFlags usage = vk::ImageUsageFlagBits::eColorAttachment;
};
inline Result<SwapchainChoice> choose_swapchain(const vk::SurfaceCapabilitiesKHR& caps,
    std::span<const vk::SurfaceFormatKHR> formats, vk::Extent2D pixels)
{
    if (!pixels.width || !pixels.height) return std::unexpected(Error{ErrorCode::invalid_state, "surface has zero pixel extent"});
    if (!(caps.supportedUsageFlags & vk::ImageUsageFlagBits::eColorAttachment) || !caps.maxImageArrayLayers ||
        (caps.maxImageCount && caps.maxImageCount < caps.minImageCount) ||
        caps.minImageExtent.width > caps.maxImageExtent.width || caps.minImageExtent.height > caps.maxImageExtent.height)
        return std::unexpected(Error{ErrorCode::not_supported, "surface lacks valid color attachment capabilities"});
    SwapchainChoice result;
    if (caps.supportedUsageFlags & vk::ImageUsageFlagBits::eTransferSrc) result.usage |= vk::ImageUsageFlagBits::eTransferSrc;
    bool found = false;
    for (auto preferred : {vk::Format::eB8G8R8A8Srgb, vk::Format::eR8G8B8A8Srgb, vk::Format::eB8G8R8A8Unorm, vk::Format::eR8G8B8A8Unorm}) {
        for (const auto& format : formats) if (format.colorSpace == vk::ColorSpaceKHR::eSrgbNonlinear &&
            (format.format == preferred || (formats.size() == 1 && format.format == vk::Format::eUndefined))) {
            result.format = {preferred, format.colorSpace}; found = true; break;
        }
        if (found) break;
    }
    if (!found) return std::unexpected(Error{ErrorCode::not_supported, "surface has no supported 8-bit sRGB/UNORM format"});
    result.extent = caps.currentExtent.width == std::numeric_limits<std::uint32_t>::max()
        ? vk::Extent2D{std::clamp(pixels.width, caps.minImageExtent.width, caps.maxImageExtent.width),
            std::clamp(pixels.height, caps.minImageExtent.height, caps.maxImageExtent.height)} : caps.currentExtent;
    if (!result.extent.width || !result.extent.height) return std::unexpected(Error{ErrorCode::invalid_state, "surface is suspended"});
    result.images = caps.minImageCount == std::numeric_limits<std::uint32_t>::max() ? caps.minImageCount : caps.minImageCount + 1;
    if (caps.maxImageCount) result.images = std::min(result.images, caps.maxImageCount);
    for (auto alpha : {vk::CompositeAlphaFlagBitsKHR::eOpaque, vk::CompositeAlphaFlagBitsKHR::ePreMultiplied,
        vk::CompositeAlphaFlagBitsKHR::ePostMultiplied, vk::CompositeAlphaFlagBitsKHR::eInherit}) {
        if (caps.supportedCompositeAlpha & alpha) { result.alpha = alpha; return result; }
    }
    return std::unexpected(Error{ErrorCode::not_supported, "surface has no composite alpha mode"});
}
inline bool present_enqueued(VkResult result) noexcept {
    return result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR || result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_ERROR_SURFACE_LOST_KHR;
}
} // namespace dk::graphics::detail

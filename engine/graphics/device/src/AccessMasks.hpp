#pragma once
#include <dk/graphics/Resources.hpp>

namespace dk::graphics::detail {
inline constexpr auto write_access = vk::AccessFlagBits2::eShaderWrite | vk::AccessFlagBits2::eShaderStorageWrite |
    vk::AccessFlagBits2::eTransferWrite | vk::AccessFlagBits2::eColorAttachmentWrite |
    vk::AccessFlagBits2::eDepthStencilAttachmentWrite | vk::AccessFlagBits2::eHostWrite | vk::AccessFlagBits2::eMemoryWrite;
inline constexpr auto read_access = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderSampledRead |
    vk::AccessFlagBits2::eShaderStorageRead | vk::AccessFlagBits2::eUniformRead | vk::AccessFlagBits2::eTransferRead |
    vk::AccessFlagBits2::eVertexAttributeRead | vk::AccessFlagBits2::eIndexRead |
    vk::AccessFlagBits2::eColorAttachmentRead | vk::AccessFlagBits2::eDepthStencilAttachmentRead |
    vk::AccessFlagBits2::eHostRead | vk::AccessFlagBits2::eMemoryRead;
} // namespace dk::graphics::detail

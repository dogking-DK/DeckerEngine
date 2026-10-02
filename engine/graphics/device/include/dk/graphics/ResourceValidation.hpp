#pragma once
#include <dk/graphics/Resources.hpp>

namespace dk::graphics {
// Pure value validation shared by recording and graph declaration. No device access.
struct AccessDescription {
    AccessState state{};
    vk::DeviceSize offset = 0, size = VK_WHOLE_SIZE;
    vk::ImageSubresourceRange range{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
    bool full_overwrite = false;
};
[[nodiscard]] Result<void> validate_buffer_description(const BufferDesc& description);
[[nodiscard]] Result<void> validate_image_description(const ImageDesc& description);
// Return a normalized copy (in particular, buffer VK_WHOLE_SIZE becomes a byte count).
[[nodiscard]] Result<AccessDescription> validate_buffer_access(const BufferDesc&, const AccessDescription&);
[[nodiscard]] Result<AccessDescription> validate_image_access(const ImageDesc&, const AccessDescription&);
[[nodiscard]] bool access_reads(vk::AccessFlags2 access) noexcept;
[[nodiscard]] bool access_writes(vk::AccessFlags2 access) noexcept;
} // namespace dk::graphics

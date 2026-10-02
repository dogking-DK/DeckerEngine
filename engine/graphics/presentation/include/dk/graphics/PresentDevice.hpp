#pragma once
#include <dk/graphics/Device.hpp>
#include <dk/platform/Window.hpp>

namespace dk::graphics {
// Overrides options.surface with the window bridge. Device retains the window.
// Only system Vulkan loader is supported with SDL's WSI loader.
[[nodiscard]] Result<Device> create_present_device(memory::ResourceHandle resource,
    const platform::Window& window, const DeviceOptions& options = {});
} // namespace dk::graphics

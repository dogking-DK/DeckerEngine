#include <dk/graphics/PresentDevice.hpp>
#include "WindowInternal.hpp"
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

namespace dk::graphics {
Result<Device> create_present_device(memory::ResourceHandle resource, const platform::Window& window, const DeviceOptions& options)
{
    if (!resource || resource.state() != memory::ResourceState::open)
        return std::unexpected(Error{ErrorCode::invalid_argument, "presentation requires an open Memory resource"});
    if (!options.loader_path.empty()) return std::unexpected(Error{ErrorCode::invalid_argument, "SDL presentation requires the system Vulkan loader"});
    auto native = platform::detail::WindowAccess::native(window);
    if (!native) return std::unexpected(native.error());
    if (!(SDL_GetWindowFlags(*native) & SDL_WINDOW_VULKAN))
        return std::unexpected(Error{ErrorCode::invalid_argument, "presentation requires a Vulkan window"});
    std::uint32_t count = 0;
    const auto* names = SDL_Vulkan_GetInstanceExtensions(&count);
    if (!names || !count) return std::unexpected(Error{ErrorCode::not_supported, std::string("SDL Vulkan extensions: ") + SDL_GetError()});
    Vector<const char*> extensions{memory::Allocator<const char*>{resource}};
    extensions.assign(names, names + count);
    auto configured = options;
    configured.surface.instance_extensions = extensions;
    configured.surface.owner = platform::detail::WindowAccess::lifetime(window);
    configured.surface.user_data = *native;
    configured.surface.create = [](VkInstance instance, void* value) -> Result<VkSurfaceKHR> {
        VkSurfaceKHR surface = VK_NULL_HANDLE;
        if (!SDL_Vulkan_CreateSurface(static_cast<SDL_Window*>(value), instance, nullptr, &surface))
            return std::unexpected(Error{ErrorCode::internal_error, std::string("SDL_Vulkan_CreateSurface: ") + SDL_GetError()});
        return surface;
    };
    return Device::create(resource, configured);
}
} // namespace dk::graphics

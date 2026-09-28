#pragma once
#include <dk/graphics/Device.hpp>

namespace dk::graphics::detail {
// Takes ownership immediately when the native calls succeed, including when
// vk-bootstrap subsequently returns an error or throws before returning handles.
struct InstanceOwner {
    InstanceOwner() = default;
    InstanceOwner(const InstanceOwner&) = delete;
    InstanceOwner& operator=(const InstanceOwner&) = delete;
    VkInstance instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
    PFN_vkDestroyInstance destroy_instance = nullptr;
    PFN_vkDestroyDebugUtilsMessengerEXT destroy_messenger = nullptr;
    ~InstanceOwner();
    void reset() noexcept;
};
Result<void> bootstrap_instance(PFN_vkGetInstanceProcAddr resolver, bool validation,
    const VkDebugUtilsMessengerCreateInfoEXT& debug, InstanceOwner& owner);
} // namespace dk::graphics::detail

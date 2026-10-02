#pragma once
#include <dk/graphics/Device.hpp>

namespace dk::graphics::detail {
// Temporary C API handoff guard, only until vk::raii has acquired the handles.
// Also covers vk-bootstrap failing/throwing without returning created handles.
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
    void adopt(const vk::raii::Context& context, vk::raii::Instance& target,
               vk::raii::DebugUtilsMessengerEXT& target_messenger);
};
Result<void> bootstrap_instance(PFN_vkGetInstanceProcAddr resolver, bool validation,
    const VkDebugUtilsMessengerCreateInfoEXT& debug, InstanceOwner& owner, std::span<const char* const> extensions = {});
} // namespace dk::graphics::detail

#include "Bootstrap.hpp"
#include <VkBootstrap.h>
#include <cstring>
#include <mutex>

namespace dk::graphics::detail {
namespace {
struct Context { PFN_vkGetInstanceProcAddr resolver; InstanceOwner& owner; };
thread_local Context* current = nullptr;
std::mutex bootstrap_mutex;
template<class T> T proc(VkInstance instance, const char* name)
{ return reinterpret_cast<T>(current->resolver(instance, name)); }
VKAPI_ATTR VkResult VKAPI_CALL version(std::uint32_t* value)
{ return proc<PFN_vkEnumerateInstanceVersion>(VK_NULL_HANDLE, "vkEnumerateInstanceVersion")(value); }
VKAPI_ATTR VkResult VKAPI_CALL layers(std::uint32_t* count, VkLayerProperties* data)
{ return proc<PFN_vkEnumerateInstanceLayerProperties>(VK_NULL_HANDLE, "vkEnumerateInstanceLayerProperties")(count, data); }
VKAPI_ATTR VkResult VKAPI_CALL extensions(const char* layer, std::uint32_t* count, VkExtensionProperties* data)
{ return proc<PFN_vkEnumerateInstanceExtensionProperties>(VK_NULL_HANDLE, "vkEnumerateInstanceExtensionProperties")(layer, count, data); }
VKAPI_ATTR VkResult VKAPI_CALL create_instance(const VkInstanceCreateInfo* info, const VkAllocationCallbacks* allocator, VkInstance* output)
{
    const auto result = proc<PFN_vkCreateInstance>(VK_NULL_HANDLE, "vkCreateInstance")(info, allocator, output);
    if (result == VK_SUCCESS) {
        current->owner.instance = *output;
        current->owner.destroy_instance = proc<PFN_vkDestroyInstance>(*output, "vkDestroyInstance");
    }
    return result;
}
VKAPI_ATTR VkResult VKAPI_CALL create_messenger(VkInstance instance, const VkDebugUtilsMessengerCreateInfoEXT* info,
    const VkAllocationCallbacks* allocator, VkDebugUtilsMessengerEXT* output)
{
    const auto create = proc<PFN_vkCreateDebugUtilsMessengerEXT>(instance, "vkCreateDebugUtilsMessengerEXT");
    const auto destroy = proc<PFN_vkDestroyDebugUtilsMessengerEXT>(instance, "vkDestroyDebugUtilsMessengerEXT");
    if (!create || !destroy) return VK_ERROR_EXTENSION_NOT_PRESENT;
    const auto result = create(instance, info, allocator, output);
    if (result == VK_SUCCESS) { current->owner.messenger = *output; current->owner.destroy_messenger = destroy; }
    return result;
}
// vk-bootstrap 1.4.357 caches these addresses process-wide. Only stable
// forwarding functions may be cached; no per-loader pointer escapes the scope.
// This adapter intentionally supports InstanceBuilder only, not other vkb APIs.
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL resolve(VkInstance, const char* name)
{
#define DK_BOOTSTRAP_PROC(vk, function) if (std::strcmp(name, #vk) == 0) return reinterpret_cast<PFN_vkVoidFunction>(function)
    DK_BOOTSTRAP_PROC(vkEnumerateInstanceVersion, version);
    DK_BOOTSTRAP_PROC(vkEnumerateInstanceLayerProperties, layers);
    DK_BOOTSTRAP_PROC(vkEnumerateInstanceExtensionProperties, extensions);
    DK_BOOTSTRAP_PROC(vkCreateInstance, create_instance);
    DK_BOOTSTRAP_PROC(vkCreateDebugUtilsMessengerEXT, create_messenger);
#undef DK_BOOTSTRAP_PROC
    return nullptr;
}
struct Scope {
    explicit Scope(Context& context) { current = &context; }
    ~Scope() { current = nullptr; }
};
}
InstanceOwner::~InstanceOwner()
{ reset(); }
void InstanceOwner::reset() noexcept
{
    if (messenger && destroy_messenger) destroy_messenger(instance, messenger, nullptr);
    if (instance && destroy_instance) destroy_instance(instance, nullptr);
    messenger = VK_NULL_HANDLE;
    instance = VK_NULL_HANDLE;
}
void InstanceOwner::adopt(const vk::raii::Context& context, vk::raii::Instance& target,
                          vk::raii::DebugUtilsMessengerEXT& target_messenger)
{
    // Instance adoption allocates a dispatcher. Retain native ownership until
    // that succeeds. Messenger adoption and the following moves do not allocate.
    vk::raii::Instance owned{context, instance};
    vk::raii::DebugUtilsMessengerEXT owned_messenger{nullptr};
    if (messenger) owned_messenger = vk::raii::DebugUtilsMessengerEXT{owned, messenger};
    instance = VK_NULL_HANDLE;
    messenger = VK_NULL_HANDLE;
    target = std::move(owned);
    target_messenger = std::move(owned_messenger);
}
Result<void> bootstrap_instance(PFN_vkGetInstanceProcAddr resolver, bool validation,
    const VkDebugUtilsMessengerCreateInfoEXT& debug, InstanceOwner& owner, std::span<const char* const> extensions)
{
    const std::lock_guard lock(bootstrap_mutex);
    Context context{resolver, owner};
    const Scope scope{context};
    vkb::InstanceBuilder builder{resolve};
    builder.set_app_name("DeckerEngine").set_engine_name("DeckerEngine").require_api_version(device_api_version).set_headless();
    for (auto* extension : extensions) builder.enable_extension(extension);
    if (validation) {
        builder.enable_validation_layers().set_debug_callback(debug.pfnUserCallback)
            .set_debug_callback_user_data_pointer(debug.pUserData)
            .set_debug_messenger_severity(debug.messageSeverity).set_debug_messenger_type(debug.messageType);
    }
    const auto result = builder.build();
    if (!result) {
        const auto operation = result.matches_error(vkb::InstanceError::failed_create_instance) ? "vkCreateInstance" :
            result.matches_error(vkb::InstanceError::failed_create_debug_messenger) ? "vkCreateDebugUtilsMessengerEXT" : "vk-bootstrap InstanceBuilder";
        const auto vk = result.vk_result();
        if (vk == VK_SUCCESS)
            return std::unexpected(Error{ErrorCode::internal_error, std::string(operation) + " failed: " + result.error().message()});
        return std::unexpected(Error{vk == VK_ERROR_INCOMPATIBLE_DRIVER ? ErrorCode::not_supported : ErrorCode::internal_error,
            std::string(operation) + " failed: " + std::string(vulkan_result_name(vk)) + " (" +
            std::to_string(static_cast<int>(vk)) + ")", {result.error().message()}});
    }
    return {};
}
} // namespace dk::graphics::detail

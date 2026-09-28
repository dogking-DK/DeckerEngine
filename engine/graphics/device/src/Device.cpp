#include "DeviceInternal.hpp"
#include "Bootstrap.hpp"
#include <dk/profiling/Profiler.hpp>
#include <volk.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace dk::graphics {
namespace {
Error vk_error(const char* operation, VkResult result)
{
    return {result == VK_ERROR_INCOMPATIBLE_DRIVER || result == VK_ERROR_LAYER_NOT_PRESENT ||
            result == VK_ERROR_EXTENSION_NOT_PRESENT || result == VK_ERROR_FEATURE_NOT_PRESENT
                ? ErrorCode::not_supported : ErrorCode::internal_error,
        std::string(operation) + " failed: " + std::string(vulkan_result_name(result)) +
        " (" + std::to_string(static_cast<int>(result)) + ")"};
}

// Enumerations may change between the count and fill calls. Never publish a
// partial VK_INCOMPLETE list; bound retries when the environment keeps changing.
template<class T, class F>
Result<Vector<T>> enumerate(memory::ResourceHandle resource, const char* operation, F&& function)
{
    Vector<T> values{memory::Allocator<T>{resource}};
    for (int attempt = 0; attempt < 8; ++attempt) {
        std::uint32_t count = 0;
        auto result = function(&count, static_cast<T*>(nullptr));
        if (result != VK_SUCCESS && result != VK_INCOMPLETE) return std::unexpected(vk_error(operation, result));
        values.resize(count);
        if (count == 0 && result == VK_SUCCESS) return values;
        result = function(&count, values.data());
        if (result == VK_INCOMPLETE) continue;
        if (result != VK_SUCCESS) return std::unexpected(vk_error(operation, result));
        values.resize(count);
        return values;
    }
    return std::unexpected(Error{ErrorCode::conflict, std::string(operation) + " did not stabilize (VK_INCOMPLETE)"});
}

struct Loader {
#ifdef _WIN32
    HMODULE module = nullptr;
#else
    void* module = nullptr;
#endif
    ~Loader()
    {
        if (!module) return;
#ifdef _WIN32
        FreeLibrary(module);
#else
        dlclose(module);
#endif
    }
    Result<PFN_vkGetInstanceProcAddr> open(const std::filesystem::path& path)
    {
#ifdef _WIN32
        module = path.empty() ? LoadLibraryExW(L"vulkan-1.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)
                              : LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        auto resolver = module ? reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(module, "vkGetInstanceProcAddr")) : nullptr;
#else
        module = dlopen(path.empty() ? "libvulkan.so.1" : path.c_str(), RTLD_NOW | RTLD_LOCAL);
        auto resolver = module ? reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(module, "vkGetInstanceProcAddr")) : nullptr;
#endif
        if (!module) return std::unexpected(Error{ErrorCode::not_found, "Vulkan loader unavailable; install a Vulkan driver or supply an absolute loader_path"});
        if (!resolver) return std::unexpected(Error{ErrorCode::internal_error, "Vulkan loader has no vkGetInstanceProcAddr"});
        return resolver;
    }
};

// volk's loading entry points use globals internally. Keep them inside a short
// serialized scope; each Device owns complete tables used outside this scope.
std::mutex volk_mutex;
void load_instance_table(PFN_vkGetInstanceProcAddr resolver, VkInstance instance, VolkInstanceTable& table)
{
    const std::lock_guard lock(volk_mutex);
    volkInitializeCustom(resolver);
    volkLoadInstanceTable(&table, instance);
    volkFinalize();
}
void load_device_table(PFN_vkGetInstanceProcAddr resolver, PFN_vkGetDeviceProcAddr device_resolver,
                       VkDevice device, VolkDeviceTable& table)
{
    const std::lock_guard lock(volk_mutex);
    volkInitializeCustom(resolver);
    ::vkGetDeviceProcAddr = device_resolver;
    volkLoadDeviceTable(&table, device);
    volkFinalize();
}
VmaVulkanFunctions allocator_functions(const VolkInstanceTable& instance, const VolkDeviceTable& device)
{
    VmaVulkanFunctions functions{};
#define DK_VMA_INSTANCE(name) functions.name = instance.name
#define DK_VMA_DEVICE(name) functions.name = device.name
    DK_VMA_INSTANCE(vkGetPhysicalDeviceProperties);
    DK_VMA_INSTANCE(vkGetPhysicalDeviceMemoryProperties);
    DK_VMA_DEVICE(vkAllocateMemory);
    DK_VMA_DEVICE(vkFreeMemory);
    DK_VMA_DEVICE(vkMapMemory);
    DK_VMA_DEVICE(vkUnmapMemory);
    DK_VMA_DEVICE(vkFlushMappedMemoryRanges);
    DK_VMA_DEVICE(vkInvalidateMappedMemoryRanges);
    DK_VMA_DEVICE(vkBindBufferMemory);
    DK_VMA_DEVICE(vkBindImageMemory);
    DK_VMA_DEVICE(vkGetBufferMemoryRequirements);
    DK_VMA_DEVICE(vkGetImageMemoryRequirements);
    DK_VMA_DEVICE(vkCreateBuffer);
    DK_VMA_DEVICE(vkDestroyBuffer);
    DK_VMA_DEVICE(vkCreateImage);
    DK_VMA_DEVICE(vkDestroyImage);
    DK_VMA_DEVICE(vkCmdCopyBuffer);
#undef DK_VMA_INSTANCE
#undef DK_VMA_DEVICE
    functions.vkGetBufferMemoryRequirements2KHR = device.vkGetBufferMemoryRequirements2;
    functions.vkGetImageMemoryRequirements2KHR = device.vkGetImageMemoryRequirements2;
    functions.vkBindBufferMemory2KHR = device.vkBindBufferMemory2;
    functions.vkBindImageMemory2KHR = device.vkBindImageMemory2;
    functions.vkGetPhysicalDeviceMemoryProperties2KHR = instance.vkGetPhysicalDeviceMemoryProperties2;
    functions.vkGetPhysicalDeviceProperties2KHR = instance.vkGetPhysicalDeviceProperties2;
    return functions;
}
} // namespace

struct Device::Impl {
    explicit Impl(memory::ResourceHandle resource) : info(resource) {}
    Loader loader;
    detail::InstanceOwner instance_owner;
    VolkInstanceTable instance_table{};
    VolkDeviceTable device_table{};
    VmaAllocator allocator = VK_NULL_HANDLE;
    detail::DeviceAccess::AllocatorApi allocator_api;
    PFN_vkGetInstanceProcAddr get_instance_proc = nullptr;
    PFN_vkGetDeviceProcAddr get_device_proc = nullptr;
    PFN_vkDestroyDevice destroy_device = nullptr;
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    std::uint32_t family = 0;
    AdapterInfo info;
    bool validation = false;
    DiagnosticSink sink = nullptr;
    void* sink_data = nullptr;
    std::atomic<std::uint64_t> errors = 0, warnings = 0;

    ~Impl()
    {
        if (allocator) allocator_api.destroy(allocator);
        if (device && destroy_device) destroy_device(device, nullptr);
        // Explicit reset keeps callback sink/counters alive through teardown.
        instance_owner.reset();
    }
    void report(const Diagnostic& diagnostic) noexcept
    {
        if (diagnostic.severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) errors.fetch_add(1, std::memory_order_relaxed);
        if (diagnostic.severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) warnings.fetch_add(1, std::memory_order_relaxed);
        if (sink) { sink(sink_data, diagnostic); return; }
        if (diagnostic.severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
            std::fprintf(stderr, "[graphics.device] %.*s: %.*s\n", static_cast<int>(diagnostic.name.size()), diagnostic.name.data(),
                         static_cast<int>(diagnostic.message.size()), diagnostic.message.data());
        }
    }
    static VKAPI_ATTR VkBool32 VKAPI_CALL callback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
        VkDebugUtilsMessageTypeFlagsEXT types, const VkDebugUtilsMessengerCallbackDataEXT* data, void* user) noexcept
    {
        auto& self = *static_cast<Impl*>(user);
        self.report({severity, types, data ? data->messageIdNumber : 0,
                     data && data->pMessageIdName ? data->pMessageIdName : "",
                     data && data->pMessage ? data->pMessage : ""});
        return VK_FALSE;
    }
};

Result<Device> Device::create(memory::ResourceHandle resource, const DeviceOptions& options)
{ return detail::DeviceAccess::create(std::move(resource), options, nullptr); }

Result<Device> detail::DeviceAccess::create(memory::ResourceHandle resource, const DeviceOptions& options,
                                           PFN_vkGetInstanceProcAddr resolver, const AllocatorApi* allocator_api)
{
    DK_PROFILE_ZONE("graphics.device.create");
    if (!resource || resource.state() != memory::ResourceState::open)
        return std::unexpected(Error{ErrorCode::invalid_argument, "device requires an open Memory resource"});
    if (!options.loader_path.empty() && !options.loader_path.is_absolute())
        return std::unexpected(Error{ErrorCode::invalid_argument, "loader_path must be absolute"});
    if (auto policy = select_validation(options.validation, true, true); !policy)
        return std::unexpected(policy.error());
    auto impl = memory::make_unique_in<Device::Impl>(resource, resource);
    if (allocator_api) impl->allocator_api = *allocator_api;
    impl->sink = options.diagnostic_sink;
    impl->sink_data = options.diagnostic_user_data;
    if (!resolver) {
        auto loaded = impl->loader.open(options.loader_path);
        if (!loaded) return std::unexpected(loaded.error());
        resolver = *loaded;
    }
    impl->get_instance_proc = resolver;
    const auto global = [&](const char* name) { return resolver(VK_NULL_HANDLE, name); };
    const auto version_fn = reinterpret_cast<PFN_vkEnumerateInstanceVersion>(global("vkEnumerateInstanceVersion"));
    std::uint32_t version = VK_API_VERSION_1_0;
    if (version_fn) {
        const auto result = version_fn(&version);
        if (result != VK_SUCCESS) return std::unexpected(vk_error("vkEnumerateInstanceVersion", result));
    }
    if (version < device_api_version) return std::unexpected(Error{ErrorCode::not_supported, "Vulkan loader must support API 1.3"});
    const auto layers_fn = reinterpret_cast<PFN_vkEnumerateInstanceLayerProperties>(global("vkEnumerateInstanceLayerProperties"));
    const auto extensions_fn = reinterpret_cast<PFN_vkEnumerateInstanceExtensionProperties>(global("vkEnumerateInstanceExtensionProperties"));
    const auto create_instance = reinterpret_cast<PFN_vkCreateInstance>(global("vkCreateInstance"));
    if (!layers_fn || !extensions_fn || !create_instance)
        return std::unexpected(Error{ErrorCode::internal_error, "Vulkan loader is missing required global entry points"});
    auto layers = enumerate<VkLayerProperties>(resource, "vkEnumerateInstanceLayerProperties", layers_fn);
    if (!layers) return std::unexpected(layers.error());
    constexpr const char* layer_name = "VK_LAYER_KHRONOS_validation";
    bool has_layer = false;
    for (const auto& layer : *layers) if (std::strcmp(layer.layerName, layer_name) == 0) has_layer = true;
    auto extensions = enumerate<VkExtensionProperties>(resource, "vkEnumerateInstanceExtensionProperties",
        [&](std::uint32_t* count, VkExtensionProperties* data) { return extensions_fn(nullptr, count, data); });
    if (!extensions) return std::unexpected(extensions.error());
    const auto has_debug = [](std::span<const VkExtensionProperties> list) {
        for (const auto& extension : list) if (std::strcmp(extension.extensionName, VK_EXT_DEBUG_UTILS_EXTENSION_NAME) == 0) return true;
        return false;
    };
    bool debug_available = has_debug(*extensions);
    if (has_layer && !debug_available && options.validation != ValidationMode::disabled) {
        auto layer_extensions = enumerate<VkExtensionProperties>(resource, "vkEnumerateInstanceExtensionProperties(validation)",
            [&](std::uint32_t* count, VkExtensionProperties* data) { return extensions_fn(layer_name, count, data); });
        if (!layer_extensions) return std::unexpected(layer_extensions.error());
        debug_available = has_debug(*layer_extensions);
    }
    const auto validation = select_validation(options.validation, has_layer, debug_available);
    if (!validation) return std::unexpected(validation.error());
    impl->validation = *validation;
    if (options.validation == ValidationMode::if_available && !impl->validation)
        impl->report({VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT, VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT, 0,
                      "validation.unavailable", "Validation disabled: VK_LAYER_KHRONOS_validation or VK_EXT_debug_utils unavailable"});

    VkDebugUtilsMessengerCreateInfoEXT debug_info{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    debug_info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT |
                                VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    debug_info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    debug_info.pfnUserCallback = Device::Impl::callback;
    debug_info.pUserData = impl.get();
    const auto bootstrapped = detail::bootstrap_instance(resolver, impl->validation, debug_info, impl->instance_owner);
    if (!bootstrapped) return std::unexpected(bootstrapped.error());
    impl->instance = impl->instance_owner.instance;
    load_instance_table(resolver, impl->instance, impl->instance_table);
    const auto instance_proc = [&](const char* name) { return resolver(impl->instance, name); };
    impl->destroy_device = reinterpret_cast<PFN_vkDestroyDevice>(instance_proc("vkDestroyDevice"));
    impl->get_device_proc = impl->instance_table.vkGetDeviceProcAddr;
    const auto physical_fn = impl->instance_table.vkEnumeratePhysicalDevices;
    const auto properties_fn = impl->instance_table.vkGetPhysicalDeviceProperties2;
    const auto features_fn = impl->instance_table.vkGetPhysicalDeviceFeatures2;
    const auto queues_fn = impl->instance_table.vkGetPhysicalDeviceQueueFamilyProperties;
    const auto create_device = impl->instance_table.vkCreateDevice;
    if (!impl->instance_owner.destroy_instance || !impl->destroy_device || !impl->get_device_proc || !physical_fn ||
        !properties_fn || !features_fn || !queues_fn || !create_device)
        return std::unexpected(Error{ErrorCode::internal_error, "Vulkan instance is missing required API 1.3 entry points"});
    auto physicals = enumerate<VkPhysicalDevice>(resource, "vkEnumeratePhysicalDevices",
        [&](std::uint32_t* count, VkPhysicalDevice* data) { return physical_fn(impl->instance, count, data); });
    if (!physicals) return std::unexpected(physicals.error());
    Vector<AdapterInfo> adapters{memory::Allocator<AdapterInfo>{resource}};
    for (const auto physical : *physicals) {
        AdapterInfo info{resource};
        VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        properties_fn(physical, &properties);
        info.properties = properties.properties;
        // Old physical devices are still reported/rejected, but must not receive
        // feature/property structures introduced after their advertised API.
        if (info.properties.apiVersion >= device_api_version) {
            properties.pNext = &info.driver;
            properties_fn(physical, &properties);
            VkPhysicalDeviceVulkan13Features features13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
            VkPhysicalDeviceVulkan12Features features12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
            features12.pNext = &features13;
            VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
            features.pNext = &features12;
            features_fn(physical, &features);
            info.timeline_semaphore = features12.timelineSemaphore == VK_TRUE;
            info.synchronization2 = features13.synchronization2 == VK_TRUE;
            info.dynamic_rendering = features13.dynamicRendering == VK_TRUE;
        }
        std::uint32_t count = 0;
        queues_fn(physical, &count, nullptr);
        info.queues.resize(count);
        if (count > 0) { queues_fn(physical, &count, info.queues.data()); info.queues.resize(count); }
        adapters.push_back(std::move(info));
    }
    auto selection = select_adapter(adapters, options.adapter_index);
    if (!selection) return std::unexpected(selection.error());
    impl->physical = (*physicals)[selection->adapter_index];
    impl->family = selection->queue_family;
    impl->info = std::move(adapters[selection->adapter_index]);
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queue_info.queueFamilyIndex = impl->family;
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = &priority;
    VkPhysicalDeviceVulkan13Features features13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    features13.synchronization2 = VK_TRUE;
    features13.dynamicRendering = VK_TRUE;
    VkPhysicalDeviceVulkan12Features features12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    features12.timelineSemaphore = VK_TRUE;
    features12.pNext = &features13;
    VkDeviceCreateInfo device_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    device_info.pNext = &features12;
    device_info.queueCreateInfoCount = 1;
    device_info.pQueueCreateInfos = &queue_info;
    auto result = create_device(impl->physical, &device_info, nullptr, &impl->device);
    if (result != VK_SUCCESS) { impl->device = VK_NULL_HANDLE; return std::unexpected(vk_error("vkCreateDevice", result)); }
    load_device_table(resolver, impl->get_device_proc, impl->device, impl->device_table);
    if (!impl->device_table.vkDestroyDevice || !impl->device_table.vkGetDeviceQueue)
        return std::unexpected(Error{ErrorCode::internal_error, "Vulkan device is missing required entry points"});
    impl->destroy_device = impl->device_table.vkDestroyDevice;
    impl->device_table.vkGetDeviceQueue(impl->device, impl->family, 0, &impl->queue);
    if (!impl->queue) return std::unexpected(Error{ErrorCode::internal_error, "vkGetDeviceQueue returned a null queue"});
    const auto functions = allocator_functions(impl->instance_table, impl->device_table);
    VmaAllocatorCreateInfo allocator_info{};
    allocator_info.instance = impl->instance;
    allocator_info.physicalDevice = impl->physical;
    allocator_info.device = impl->device;
    // M5.1 does not enable maintenance4. VMA's 1.2 path only uses features
    // already enabled, even though the underlying device API is 1.3.
    allocator_info.vulkanApiVersion = VK_API_VERSION_1_2;
    allocator_info.pVulkanFunctions = &functions;
    result = impl->allocator_api.create(&allocator_info, &impl->allocator);
    if (result != VK_SUCCESS) { impl->allocator = VK_NULL_HANDLE; return std::unexpected(vk_error("vmaCreateAllocator", result)); }
    return Device{std::move(impl)};
}

Device::Device(memory::UniquePtr<Impl> impl) noexcept : impl_(std::move(impl)) {}
Device::~Device() = default;
Device::Device(Device&&) noexcept = default;
Device& Device::operator=(Device&&) noexcept = default;
VkInstance Device::instance() const noexcept { return impl_->instance; }
VkPhysicalDevice Device::physical_device() const noexcept { return impl_->physical; }
VkDevice Device::native_device() const noexcept { return impl_->device; }
VkQueue Device::queue() const noexcept { return impl_->queue; }
VmaAllocator Device::allocator() const noexcept { return impl_->allocator; }
std::uint32_t Device::queue_family() const noexcept { return impl_->family; }
const AdapterInfo& Device::adapter() const noexcept { return impl_->info; }
bool Device::validation_enabled() const noexcept { return impl_->validation; }
std::uint64_t Device::validation_errors() const noexcept { return impl_->errors.load(std::memory_order_relaxed); }
std::uint64_t Device::validation_warnings() const noexcept { return impl_->warnings.load(std::memory_order_relaxed); }
PFN_vkVoidFunction Device::instance_proc(const char* name) const noexcept { return name ? impl_->get_instance_proc(impl_->instance, name) : nullptr; }
PFN_vkVoidFunction Device::device_proc(const char* name) const noexcept { return name ? impl_->get_device_proc(impl_->device, name) : nullptr; }
} // namespace dk::graphics

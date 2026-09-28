#include <dk/graphics/Device.hpp>
#include <dk/memory/MemorySystem.hpp>
#include "DeviceInternal.hpp"
#include <catch2/catch_test_macros.hpp>
#include <array>
#include <cstring>

using namespace dk;
using namespace dk::graphics;
namespace {
template<std::size_t N, std::size_t M> void copy_name(char (&target)[N], const char (&source)[M])
{ static_assert(M <= N); std::memcpy(target, source, M); }
struct Memory {
    memory::MemorySystem system = [] { auto result = memory::MemorySystem::create(); REQUIRE(result); return std::move(*result); }();
    memory::ResourceHandle resource = [&] { auto result = system.create_heap({"device-tests", memory::DomainCategory::render}); REQUIRE(result); return *result; }();
};
AdapterInfo suitable(memory::ResourceHandle resource, VkPhysicalDeviceType type = VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
{
    AdapterInfo info{resource};
    info.properties.apiVersion = device_api_version;
    info.properties.deviceType = type;
    copy_name(info.properties.deviceName, "test adapter");
    info.timeline_semaphore = info.synchronization2 = info.dynamic_rendering = true;
    info.queues.push_back({VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT, 1, 64, {1, 1, 1}});
    return info;
}
template<class T> T handle(std::uintptr_t value) { return reinterpret_cast<T>(value); }
struct Fake {
    inline static Fake* active = nullptr;
    Fake() { REQUIRE(active == nullptr); active = this; }
    ~Fake() { active = nullptr; }
    VkResult instance_result = VK_SUCCESS, device_result = VK_SUCCESS, messenger_result = VK_SUCCESS;
    VkResult allocator_result = VK_SUCCESS;
    VkResult enumerate_result = VK_SUCCESS;
    bool empty = false, null_queue = false, incomplete_once = false, incomplete_always = false;
    bool layer = true, debug = true;
    std::uint32_t loader_version = device_api_version;
    memory::ResourceHandle close_resource;
    int fills = 0, creates = 0, alternate_creates = 0;
    std::string destroyed;
    VkDebugUtilsMessengerCreateInfoEXT callback_info{};
    bool feature_contract = false;
    static VKAPI_ATTR VkResult VKAPI_CALL version(std::uint32_t* value) { *value = active->loader_version; return VK_SUCCESS; }
    static VKAPI_ATTR VkResult VKAPI_CALL layers(std::uint32_t* count, VkLayerProperties* data)
    { *count = active->layer ? 1u : 0u; if (data && active->layer) copy_name(data[0].layerName, "VK_LAYER_KHRONOS_validation"); return VK_SUCCESS; }
    static VKAPI_ATTR VkResult VKAPI_CALL extensions(const char*, std::uint32_t* count, VkExtensionProperties* data)
    { *count = active->debug ? 1u : 0u; if (data && active->debug) copy_name(data[0].extensionName, VK_EXT_DEBUG_UTILS_EXTENSION_NAME); return VK_SUCCESS; }
    static VKAPI_ATTR VkResult VKAPI_CALL instance(const VkInstanceCreateInfo* info, const VkAllocationCallbacks*, VkInstance* value)
    {
        ++active->creates;
        if (info->pNext) active->callback_info = *static_cast<const VkDebugUtilsMessengerCreateInfoEXT*>(info->pNext);
        if (active->instance_result == VK_SUCCESS) *value = handle<VkInstance>(1);
        return active->instance_result;
    }
    static VKAPI_ATTR void VKAPI_CALL destroy_instance(VkInstance, const VkAllocationCallbacks*) { active->destroyed += 'I'; }
    static VKAPI_ATTR void VKAPI_CALL destroy_device(VkDevice, const VkAllocationCallbacks*) { active->destroyed += 'D'; }
    static VKAPI_ATTR void VKAPI_CALL destroy_messenger(VkInstance, VkDebugUtilsMessengerEXT, const VkAllocationCallbacks*) { active->destroyed += 'M'; }
    static VKAPI_ATTR VkResult VKAPI_CALL messenger(VkInstance, const VkDebugUtilsMessengerCreateInfoEXT* info, const VkAllocationCallbacks*, VkDebugUtilsMessengerEXT* value)
    {
        active->callback_info = *info;
        if (active->messenger_result == VK_SUCCESS) *value = handle<VkDebugUtilsMessengerEXT>(4);
        return active->messenger_result;
    }
    static VKAPI_ATTR VkResult VKAPI_CALL physicals(VkInstance, std::uint32_t* count, VkPhysicalDevice* data)
    {
        *count = active->empty ? 0u : 1u;
        if (data && !active->empty) {
            data[0] = handle<VkPhysicalDevice>(2);
            ++active->fills;
            if (active->incomplete_always || (active->incomplete_once && active->fills == 1)) return VK_INCOMPLETE;
        }
        return active->enumerate_result;
    }
    static VKAPI_ATTR void VKAPI_CALL properties(VkPhysicalDevice, VkPhysicalDeviceProperties2* value)
    {
        value->properties.apiVersion = device_api_version;
        copy_name(value->properties.deviceName, "fake GPU");
        if (active->close_resource) active->close_resource.begin_close();
    }
    static VKAPI_ATTR void VKAPI_CALL features(VkPhysicalDevice, VkPhysicalDeviceFeatures2* value)
    {
        auto* f12 = static_cast<VkPhysicalDeviceVulkan12Features*>(value->pNext);
        auto* f13 = static_cast<VkPhysicalDeviceVulkan13Features*>(f12->pNext);
        f12->timelineSemaphore = VK_TRUE;
        f13->synchronization2 = f13->dynamicRendering = VK_TRUE;
    }
    static VKAPI_ATTR void VKAPI_CALL queues(VkPhysicalDevice, std::uint32_t* count, VkQueueFamilyProperties* data)
    { *count = 1; if (data) data[0] = {VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT, 1, 64, {1, 1, 1}}; }
    static VKAPI_ATTR VkResult VKAPI_CALL device(VkPhysicalDevice, const VkDeviceCreateInfo* info, const VkAllocationCallbacks*, VkDevice* value)
    {
        const auto* f12 = static_cast<const VkPhysicalDeviceVulkan12Features*>(info->pNext);
        const auto* f13 = static_cast<const VkPhysicalDeviceVulkan13Features*>(f12->pNext);
        active->feature_contract = f12->timelineSemaphore && f13->synchronization2 && f13->dynamicRendering &&
            !f12->bufferDeviceAddress && !f13->maintenance4 && info->enabledExtensionCount == 0 &&
            info->queueCreateInfoCount == 1 && info->pQueueCreateInfos->queueCount == 1 &&
            info->pQueueCreateInfos->queueFamilyIndex == 0 && *info->pQueueCreateInfos->pQueuePriorities == 1.0f;
        if (active->device_result == VK_SUCCESS) *value = handle<VkDevice>(3);
        return active->device_result;
    }
    static VKAPI_ATTR void VKAPI_CALL queue(VkDevice, std::uint32_t, std::uint32_t, VkQueue* value)
    { *value = active->null_queue ? VK_NULL_HANDLE : handle<VkQueue>(5); }
    static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL device_proc(VkDevice, const char* name) { return resolve(VK_NULL_HANDLE, name); }
    static VkResult create_allocator(const VmaAllocatorCreateInfo* info, VmaAllocator* output)
    {
        REQUIRE(info->instance != VK_NULL_HANDLE);
        REQUIRE(info->device != VK_NULL_HANDLE);
        REQUIRE(info->vulkanApiVersion == VK_API_VERSION_1_2);
        if (active->allocator_result == VK_SUCCESS) *output = handle<VmaAllocator>(6);
        return active->allocator_result;
    }
    static void destroy_allocator(VmaAllocator) { active->destroyed += 'A'; }
    static VKAPI_ATTR VkResult VKAPI_CALL alternate_instance(const VkInstanceCreateInfo*, const VkAllocationCallbacks*, VkInstance*)
    { ++active->alternate_creates; return VK_ERROR_INITIALIZATION_FAILED; }
    static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL alternate_resolve(VkInstance instance_handle, const char* name)
    {
        if (std::strcmp(name, "vkCreateInstance") == 0) return reinterpret_cast<PFN_vkVoidFunction>(alternate_instance);
        return resolve(instance_handle, name);
    }
    static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL resolve(VkInstance, const char* name)
    {
#define DK_FAKE_PROC(vk, function) if (std::strcmp(name, #vk) == 0) return reinterpret_cast<PFN_vkVoidFunction>(function)
        DK_FAKE_PROC(vkEnumerateInstanceVersion, version);
        DK_FAKE_PROC(vkEnumerateInstanceLayerProperties, layers);
        DK_FAKE_PROC(vkEnumerateInstanceExtensionProperties, extensions);
        DK_FAKE_PROC(vkCreateInstance, instance);
        DK_FAKE_PROC(vkDestroyInstance, destroy_instance);
        DK_FAKE_PROC(vkCreateDebugUtilsMessengerEXT, messenger);
        DK_FAKE_PROC(vkDestroyDebugUtilsMessengerEXT, destroy_messenger);
        DK_FAKE_PROC(vkEnumeratePhysicalDevices, physicals);
        DK_FAKE_PROC(vkGetPhysicalDeviceProperties2, properties);
        DK_FAKE_PROC(vkGetPhysicalDeviceFeatures2, features);
        DK_FAKE_PROC(vkGetPhysicalDeviceQueueFamilyProperties, queues);
        DK_FAKE_PROC(vkCreateDevice, device);
        DK_FAKE_PROC(vkDestroyDevice, destroy_device);
        DK_FAKE_PROC(vkGetDeviceQueue, queue);
        DK_FAKE_PROC(vkGetDeviceProcAddr, device_proc);
#undef DK_FAKE_PROC
        return nullptr;
    }
    Result<Device> create(memory::ResourceHandle resource, DeviceOptions options = {}, PFN_vkGetInstanceProcAddr resolver = resolve)
    {
        const graphics::detail::DeviceAccess::AllocatorApi api{create_allocator, destroy_allocator};
        return graphics::detail::DeviceAccess::create(resource, options, resolver, &api);
    }
};
}

TEST_CASE("adapter selection ranks suitable devices and honors explicit choice")
{
    Memory memory;
    std::array adapters{suitable(memory.resource, VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU), suitable(memory.resource), suitable(memory.resource)};
    REQUIRE(select_adapter(adapters)->adapter_index == 1);
    REQUIRE(select_adapter(adapters, 0)->adapter_index == 0);
    adapters[1].dynamic_rendering = false;
    REQUIRE(select_adapter(adapters)->adapter_index == 2);
    auto rejected = select_adapter(adapters, 1);
    REQUIRE_FALSE(rejected);
    REQUIRE(rejected.error().context[0].find("dynamicRendering") != std::string::npos);
    REQUIRE(select_adapter(adapters, 99).error().code == ErrorCode::not_found);
    REQUIRE(select_adapter({}).error().message.find("no Vulkan physical devices") != std::string::npos);
}

TEST_CASE("adapter rejection explains every missing requirement")
{
    Memory memory;
    std::array adapters{suitable(memory.resource)};
    auto& adapter = adapters[0];
    adapter.properties.apiVersion = VK_API_VERSION_1_2;
    adapter.timeline_semaphore = adapter.synchronization2 = adapter.dynamic_rendering = false;
    adapter.queues[0].queueCount = 0;
    auto result = select_adapter(adapters);
    REQUIRE_FALSE(result);
    const auto& reasons = result.error().context[0];
    for (const auto* expected : {"Vulkan 1.3", "timelineSemaphore", "synchronization2", "dynamicRendering", "graphics+compute queue"})
        REQUIRE(reasons.find(expected) != std::string::npos);
}

TEST_CASE("queue selection requires one graphics compute family and allows implicit transfer")
{
    Memory memory;
    std::array adapters{suitable(memory.resource)};
    auto& queues = adapters[0].queues;
    queues[0].queueFlags = VK_QUEUE_GRAPHICS_BIT;
    queues.push_back({VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT, 1, 0, {1, 1, 1}});
    REQUIRE_FALSE(select_adapter(adapters));
    queues.push_back({VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT, 1, 0, {1, 1, 1}});
    REQUIRE(select_adapter(adapters)->queue_family == 2);
}

TEST_CASE("validation policy distinguishes required optional and disabled")
{
    for (bool layer : {false, true}) for (bool debug : {false, true}) {
        REQUIRE(*select_validation(ValidationMode::disabled, layer, debug) == false);
        REQUIRE(*select_validation(ValidationMode::if_available, layer, debug) == (layer && debug));
        REQUIRE(select_validation(ValidationMode::required, layer, debug).has_value() == (layer && debug));
    }
    REQUIRE_FALSE(select_validation(static_cast<ValidationMode>(99), true, true));
}

TEST_CASE("device input and missing loader errors are reproducible without GPU")
{
    Memory memory;
    REQUIRE(Device::create({}).error().code == ErrorCode::invalid_argument);
    DeviceOptions options;
    options.loader_path = "relative-vulkan.dll";
    REQUIRE(Device::create(memory.resource, options).error().code == ErrorCode::invalid_argument);
    options.loader_path = std::filesystem::current_path() / "dk-nonexistent-loader" / "vulkan-missing.dll";
    REQUIRE(Device::create(memory.resource, options).error().code == ErrorCode::not_found);
    REQUIRE(memory.resource.snapshot().live_allocations == 0);
}

TEST_CASE("device cleans instance and messenger on absent devices and creation failures")
{
    Memory memory;
    Fake fake;
    std::string expected = "MI";
    std::string message;
    SECTION("no physical devices") { fake.empty = true; message = "no Vulkan physical devices"; }
    SECTION("instance fails") { fake.instance_result = VK_ERROR_INCOMPATIBLE_DRIVER; expected = ""; message = "vkCreateInstance failed: VK_ERROR_INCOMPATIBLE_DRIVER (-9)"; }
    SECTION("messenger fails") { fake.messenger_result = VK_ERROR_OUT_OF_HOST_MEMORY; expected = "I"; message = "vkCreateDebugUtilsMessengerEXT"; }
    SECTION("enumeration fails") { fake.enumerate_result = VK_ERROR_INITIALIZATION_FAILED; message = "vkEnumeratePhysicalDevices"; }
    SECTION("device fails") { fake.device_result = VK_ERROR_OUT_OF_DEVICE_MEMORY; message = "vkCreateDevice failed: VK_ERROR_OUT_OF_DEVICE_MEMORY (-2)"; }
    SECTION("queue missing") { fake.null_queue = true; expected = "DMI"; message = "vkGetDeviceQueue"; }
    SECTION("allocator fails") { fake.allocator_result = VK_ERROR_OUT_OF_HOST_MEMORY; expected = "DMI"; message = "vmaCreateAllocator failed: VK_ERROR_OUT_OF_HOST_MEMORY (-1)"; }
    auto result = fake.create(memory.resource);
    REQUIRE_FALSE(result);
    REQUIRE(result.error().message.find(message) != std::string::npos);
    REQUIRE(fake.destroyed == expected);
    REQUIRE(memory.resource.snapshot().live_allocations == 0);
}

TEST_CASE("device bounds changing enumeration and retries incomplete lists")
{
    Memory memory;
    Fake fake;
    SECTION("recovers") {
        fake.incomplete_once = true;
        { auto result = fake.create(memory.resource); REQUIRE(result); REQUIRE(fake.fills == 2); }
        REQUIRE(fake.destroyed == "ADMI");
    }
    SECTION("bounded failure") {
        fake.incomplete_always = true;
        auto result = fake.create(memory.resource);
        REQUIRE_FALSE(result);
        REQUIRE(result.error().code == ErrorCode::conflict);
        REQUIRE(fake.fills == 8);
        REQUIRE(fake.destroyed == "MI");
    }
    REQUIRE(memory.resource.snapshot().live_allocations == 0);
}

TEST_CASE("device rejects unsupported loader and missing required validation before instance")
{
    Memory memory;
    Fake fake;
    DeviceOptions options;
    options.validation = ValidationMode::required;
    SECTION("old loader") { fake.loader_version = VK_API_VERSION_1_2; }
    SECTION("missing layer") { fake.layer = false; }
    SECTION("missing debug utils") { fake.debug = false; }
    auto result = fake.create(memory.resource, options);
    REQUIRE_FALSE(result);
    REQUIRE(result.error().code == ErrorCode::not_supported);
    REQUIRE(fake.creates == 0);
    REQUIRE(memory.resource.snapshot().live_allocations == 0);
}

TEST_CASE("device reports optional validation absence")
{
    Memory memory;
    Fake fake;
    fake.layer = false;
    int diagnostics = 0;
    DeviceOptions options;
    options.diagnostic_user_data = &diagnostics;
    options.diagnostic_sink = [](void* data, const Diagnostic& message) noexcept {
        if (message.name == "validation.unavailable") ++*static_cast<int*>(data);
    };
    { auto result = fake.create(memory.resource, options); REQUIRE(result); REQUIRE_FALSE(result->validation_enabled()); }
    REQUIRE(diagnostics == 1);
    REQUIRE(fake.destroyed == "ADI");
}

TEST_CASE("device move retains stable callbacks and destroys in reverse order")
{
    Memory memory;
    Fake fake;
    int diagnostic_id = 0;
    DeviceOptions options;
    options.diagnostic_user_data = &diagnostic_id;
    options.diagnostic_sink = [](void* data, const Diagnostic& message) noexcept { *static_cast<int*>(data) = message.id; };
    {
        auto result = fake.create(memory.resource, options); REQUIRE(result);
        auto moved = std::move(*result);
        REQUIRE(fake.feature_contract);
        REQUIRE(moved.queue() != VK_NULL_HANDLE);
        VkDebugUtilsMessengerCallbackDataEXT data{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CALLBACK_DATA_EXT};
        data.messageIdNumber = 42; data.pMessageIdName = "test"; data.pMessage = "test diagnostic";
        REQUIRE(fake.callback_info.pfnUserCallback(VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
            VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT, &data, fake.callback_info.pUserData) == VK_FALSE);
        REQUIRE(moved.validation_errors() == 1);
        REQUIRE(diagnostic_id == 42);
    }
    REQUIRE(fake.destroyed == "ADMI");
    REQUIRE(memory.resource.snapshot().live_allocations == 0);
}

TEST_CASE("allocation exception after instance creation cleans native resources")
{
    Memory memory;
    Fake fake;
    fake.close_resource = memory.resource;
    REQUIRE_THROWS_AS(fake.create(memory.resource), std::bad_alloc);
    REQUIRE(fake.destroyed == "MI");
    REQUIRE(memory.resource.snapshot().live_allocations == 0);
}

TEST_CASE("bootstrap uses current loader after an earlier instance was destroyed")
{
    Memory memory;
    Fake fake;
    DeviceOptions options;
    options.validation = ValidationMode::disabled;
    { auto first = fake.create(memory.resource, options); REQUIRE(first); }
    options.validation = ValidationMode::required;
    const auto second = fake.create(memory.resource, options, Fake::alternate_resolve);
    REQUIRE_FALSE(second);
    REQUIRE(second.error().message.find("VK_ERROR_INITIALIZATION_FAILED") != std::string::npos);
    REQUIRE(fake.creates == 1);
    REQUIRE(fake.alternate_creates == 1);
    REQUIRE(fake.destroyed == "ADI");
    REQUIRE(memory.resource.snapshot().live_allocations == 0);
}

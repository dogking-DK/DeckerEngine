#include <dk/graphics/Device.hpp>
#include <dk/memory/MemorySystem.hpp>
#include "DeviceInternal.hpp"
#include <catch2/catch_test_macros.hpp>
#include <array>
#include <cstring>

using namespace dk;
using namespace dk::graphics;
namespace {
template<class T, std::size_t M> void copy_name(T& target, const char (&source)[M])
{ static_assert(M <= sizeof(T)); std::memcpy(std::data(target), source, M); }
struct Memory {
    memory::MemorySystem system = [] { auto result = memory::MemorySystem::create(); REQUIRE(result); return std::move(*result); }();
    memory::ResourceHandle resource = [&] { auto result = system.create_heap({"device-tests", memory::DomainCategory::render}); REQUIRE(result); return *result; }();
};
AdapterInfo suitable(memory::ResourceHandle resource, vk::PhysicalDeviceType type = vk::PhysicalDeviceType::eDiscreteGpu)
{
    AdapterInfo info{resource};
    info.properties.apiVersion = device_api_version;
    info.properties.deviceType = type;
    copy_name(info.properties.deviceName, "test adapter");
    info.timeline_semaphore = info.synchronization2 = info.dynamic_rendering = info.maintenance4 = true;
    info.queues.push_back({vk::QueueFlagBits::eGraphics | vk::QueueFlagBits::eCompute, 1, 64, {1, 1, 1}});
    return info;
}
TEST_CASE("presentation selection checks every queue and required extension")
{
    Memory memory;
    std::array<AdapterInfo, 2> adapters{suitable(memory.resource), suitable(memory.resource, vk::PhysicalDeviceType::eIntegratedGpu)};
    auto& candidate = adapters[1];
    candidate.queues.push_back(candidate.queues.front());
    candidate.present_queues = {0, 1};
    candidate.swapchain = candidate.swapchain_maintenance1 = true;
    auto selected = select_adapter(adapters, {}, true);
    REQUIRE(selected);
    CHECK(selected->adapter_index == 1);
    CHECK(selected->queue_family == 1);
    CHECK_FALSE(select_adapter(adapters, 0, true));
    candidate.swapchain_maintenance1 = false;
    CHECK_FALSE(select_adapter(adapters, {}, true));
    CHECK(select_adapter(adapters)->adapter_index == 0); // Headless unchanged.
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
    bool throw_instance_adoption = false, throw_device_adoption = false, teardown_callback = false;
    int device_queue_lookups = 0;
    std::uint32_t available_queues=1,requested_queues=0;
    std::uint32_t loader_version = device_api_version;
    std::uint32_t physical_version = device_api_version;
    bool maintenance4 = true, buffer_requirements = true, image_requirements = true;
    int feature_queries = 0, device_creates = 0;
    std::stop_source* cancel = nullptr;
    bool cancel_at_device = false;
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
        REQUIRE(info->pApplicationInfo->apiVersion == VK_API_VERSION_1_4);
        if (info->pNext) active->callback_info = *static_cast<const VkDebugUtilsMessengerCreateInfoEXT*>(info->pNext);
        if (active->instance_result == VK_SUCCESS) *value = handle<VkInstance>(1);
        if (active->cancel && !active->cancel_at_device) active->cancel->request_stop();
        return active->instance_result;
    }
    static VKAPI_ATTR void VKAPI_CALL destroy_instance(VkInstance, const VkAllocationCallbacks*)
    {
        if (active->teardown_callback) {
            VkDebugUtilsMessengerCallbackDataEXT data{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CALLBACK_DATA_EXT};
            data.messageIdNumber = 99;
            active->callback_info.pfnUserCallback(VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT,
                VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT, &data, active->callback_info.pUserData);
        }
        active->destroyed += 'I';
    }
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
        value->properties.apiVersion = active->physical_version;
        copy_name(value->properties.deviceName, "fake GPU");
        if (active->close_resource) active->close_resource.begin_close();
    }
    static VKAPI_ATTR void VKAPI_CALL features(VkPhysicalDevice, VkPhysicalDeviceFeatures2* value)
    {
        ++active->feature_queries;
        auto* f12 = static_cast<VkPhysicalDeviceVulkan12Features*>(value->pNext);
        auto* f13 = static_cast<VkPhysicalDeviceVulkan13Features*>(f12->pNext);
        f12->timelineSemaphore = VK_TRUE;
        f13->synchronization2 = f13->dynamicRendering = VK_TRUE;
        f13->maintenance4 = active->maintenance4 ? VK_TRUE : VK_FALSE;
    }
    static VKAPI_ATTR void VKAPI_CALL queues(VkPhysicalDevice, std::uint32_t* count, VkQueueFamilyProperties* data)
    { *count = 1; if (data) data[0] = {VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT, active->available_queues, 64, {1, 1, 1}}; }
    static VKAPI_ATTR VkResult VKAPI_CALL device(VkPhysicalDevice, const VkDeviceCreateInfo* info, const VkAllocationCallbacks*, VkDevice* value)
    {
        ++active->device_creates;
        active->requested_queues=info->pQueueCreateInfos->queueCount;
        const auto* f12 = static_cast<const VkPhysicalDeviceVulkan12Features*>(info->pNext);
        const auto* f13 = static_cast<const VkPhysicalDeviceVulkan13Features*>(f12->pNext);
        active->feature_contract = f12->timelineSemaphore && f13->synchronization2 && f13->dynamicRendering &&
            !f12->bufferDeviceAddress && f13->maintenance4 && info->enabledExtensionCount == 0 &&
            info->queueCreateInfoCount == 1 && info->pQueueCreateInfos->queueCount == 1 &&
            info->pQueueCreateInfos->queueFamilyIndex == 0 && *info->pQueueCreateInfos->pQueuePriorities == 1.0f;
        if (active->device_result == VK_SUCCESS) *value = handle<VkDevice>(3);
        if (active->cancel && active->cancel_at_device) active->cancel->request_stop();
        return active->device_result;
    }
    static VKAPI_ATTR void VKAPI_CALL queue(VkDevice, std::uint32_t, std::uint32_t index, VkQueue* value)
    { *value = active->null_queue ? VK_NULL_HANDLE : handle<VkQueue>(5+index); }
    static VKAPI_ATTR void VKAPI_CALL buffer_memory_requirements(VkDevice, const VkDeviceBufferMemoryRequirements*, VkMemoryRequirements2*) {}
    static VKAPI_ATTR void VKAPI_CALL image_memory_requirements(VkDevice, const VkDeviceImageMemoryRequirements*, VkMemoryRequirements2*) {}
    static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL device_proc(VkDevice, const char* name)
    {
        if (std::strcmp(name, "vkGetDeviceQueue") == 0 && ++active->device_queue_lookups == 2 && active->throw_device_adoption)
            throw std::bad_alloc{}; // Second pass is Hpp adoption, after volk filled its table.
        return resolve(VK_NULL_HANDLE, name);
    }
    static VkResult create_allocator(const VmaAllocatorCreateInfo* info, VmaAllocator* output)
    {
        REQUIRE(info->instance != VK_NULL_HANDLE);
        REQUIRE(info->device != VK_NULL_HANDLE);
        REQUIRE(info->vulkanApiVersion == VK_API_VERSION_1_4);
        REQUIRE(info->pVulkanFunctions->vkGetDeviceBufferMemoryRequirements == buffer_memory_requirements);
        REQUIRE(info->pVulkanFunctions->vkGetDeviceImageMemoryRequirements == image_memory_requirements);
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
        if (std::strcmp(name, "vkGetDeviceBufferMemoryRequirements") == 0)
            return active->buffer_requirements ? reinterpret_cast<PFN_vkVoidFunction>(buffer_memory_requirements) : nullptr;
        if (std::strcmp(name, "vkGetDeviceImageMemoryRequirements") == 0)
            return active->image_requirements ? reinterpret_cast<PFN_vkVoidFunction>(image_memory_requirements) : nullptr;
        if (active->throw_instance_adoption && std::strcmp(name, "vkGetPhysicalDeviceFeatures") == 0)
            throw std::bad_alloc{}; // Simulate allocation failure while constructing the Hpp dispatcher.
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

TEST_CASE("device creation cancellation unwinds only acquired native owners")
{
    for (int phase=0; phase<3; ++phase) {
        Memory memory;
        Fake fake;
        std::stop_source stop;
        if (phase==0) stop.request_stop();
        else { fake.cancel=&stop; fake.cancel_at_device=phase==2; }
        auto result=fake.create(memory.resource, {.cancel=stop.get_token()});
        REQUIRE_FALSE(result);
        REQUIRE(result.error().code==ErrorCode::invalid_state);
        REQUIRE(result.error().context==std::vector<std::string>{"graphics.device.create.cancelled"});
        REQUIRE(fake.creates==(phase==0 ? 0 : 1));
        REQUIRE(fake.device_creates==(phase==2 ? 1 : 0));
        REQUIRE(fake.destroyed==(phase==0 ? "" : phase==1 ? "MI" : "DMI"));
    }
}

TEST_CASE("adapter selection ranks suitable devices and honors explicit choice")
{
    Memory memory;
    std::array adapters{suitable(memory.resource, vk::PhysicalDeviceType::eIntegratedGpu), suitable(memory.resource), suitable(memory.resource)};
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
    adapter.properties.apiVersion = VK_API_VERSION_1_3;
    adapter.timeline_semaphore = adapter.synchronization2 = adapter.dynamic_rendering = adapter.maintenance4 = false;
    adapter.queues[0].queueCount = 0;
    auto result = select_adapter(adapters);
    REQUIRE_FALSE(result);
    const auto& reasons = result.error().context[0];
    for (const auto* expected : {"Vulkan 1.4", "timelineSemaphore", "synchronization2", "dynamicRendering", "maintenance4", "graphics+compute queue"})
        REQUIRE(reasons.find(expected) != std::string::npos);
}

TEST_CASE("queue selection requires one graphics compute family and allows implicit transfer")
{
    Memory memory;
    std::array adapters{suitable(memory.resource)};
    auto& queues = adapters[0].queues;
    queues[0].queueFlags = vk::QueueFlagBits::eGraphics;
    queues.push_back({vk::QueueFlagBits::eCompute | vk::QueueFlagBits::eTransfer, 1, 0, {1, 1, 1}});
    REQUIRE_FALSE(select_adapter(adapters));
    queues.push_back({vk::QueueFlagBits::eGraphics | vk::QueueFlagBits::eCompute, 1, 0, {1, 1, 1}});
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
    SECTION("buffer requirements missing") { fake.buffer_requirements = false; expected = "DMI"; message = "maintenance4 memory requirement entry points"; }
    SECTION("image requirements missing") { fake.image_requirements = false; expected = "DMI"; message = "maintenance4 memory requirement entry points"; }
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
    SECTION("old loader") { fake.loader_version = VK_MAKE_API_VERSION(0, 1, 3, 4095); }
    SECTION("different variant") { fake.loader_version = VK_MAKE_API_VERSION(1, 1, 4, 0); }
    SECTION("missing layer") { fake.layer = false; }
    SECTION("missing debug utils") { fake.debug = false; }
    auto result = fake.create(memory.resource, options);
    REQUIRE_FALSE(result);
    REQUIRE(result.error().code == ErrorCode::not_supported);
    REQUIRE(fake.creates == 0);
    REQUIRE(memory.resource.snapshot().live_allocations == 0);
}

TEST_CASE("device enforces Vulkan 1.4 without requiring the header patch version")
{
    Memory memory;
    Fake fake;
    bool accepted = true;
    SECTION("1.4.0 baseline") { fake.loader_version = fake.physical_version = VK_API_VERSION_1_4; }
    SECTION("1.4 newer patch") { fake.physical_version = VK_MAKE_API_VERSION(0, 1, 4, 1); }
    SECTION("1.3 device rejected") { fake.physical_version = VK_MAKE_API_VERSION(0, 1, 3, 4095); accepted = false; }
    SECTION("different device variant") { fake.physical_version = VK_MAKE_API_VERSION(1, 1, 4, 0); accepted = false; }
    SECTION("maintenance4 missing") { fake.maintenance4 = false; accepted = false; }
    {
        auto result = fake.create(memory.resource);
        REQUIRE(result.has_value() == accepted);
        if (accepted) {
            REQUIRE(fake.feature_contract);
            REQUIRE(result->adapter().maintenance4);
        } else {
            REQUIRE(result.error().code == ErrorCode::not_supported);
            REQUIRE(fake.device_creates == 0);
            const bool compatible = VK_API_VERSION_VARIANT(fake.physical_version) == 0 && fake.physical_version >= VK_API_VERSION_1_4;
            REQUIRE(fake.feature_queries == (compatible ? 1 : 0));
            REQUIRE(result.error().context[0].find(compatible ? "maintenance4" : "Vulkan 1.4") != std::string::npos);
        }
    }
    REQUIRE(fake.destroyed == (accepted ? "ADMI" : "MI"));
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
    fake.teardown_callback = true;
    int diagnostic_id = 0;
    DeviceOptions options;
    options.diagnostic_user_data = &diagnostic_id;
    options.diagnostic_sink = [](void* data, const Diagnostic& message) noexcept { *static_cast<int*>(data) = message.id; };
    {
        auto result = fake.create(memory.resource, options); REQUIRE(result);
        auto moved = std::move(*result);
        REQUIRE(fake.feature_contract);
        REQUIRE(static_cast<bool>(*moved.queue()));
        VkDebugUtilsMessengerCallbackDataEXT data{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CALLBACK_DATA_EXT};
        data.messageIdNumber = 42; data.pMessageIdName = "test"; data.pMessage = "test diagnostic";
        REQUIRE(fake.callback_info.pfnUserCallback(VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
            VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT, &data, fake.callback_info.pUserData) == VK_FALSE);
        REQUIRE(moved.validation_errors() == 1);
        REQUIRE(diagnostic_id == 42);
    }
    REQUIRE(fake.destroyed == "ADMI");
    REQUIRE(diagnostic_id == 99);
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

TEST_CASE("Hpp adoption exceptions release native handles exactly once")
{
    Memory memory;
    Fake fake;
    std::string expected;
    SECTION("instance dispatcher") { fake.throw_instance_adoption = true; expected = "MI"; }
    SECTION("device dispatcher") { fake.throw_device_adoption = true; expected = "DMI"; }
    REQUIRE_THROWS_AS(fake.create(memory.resource), std::bad_alloc);
    REQUIRE(fake.destroyed == expected);
    REQUIRE(memory.resource.snapshot().live_allocations == 0);
}

TEST_CASE("device move assignment releases previous owner and preserves RAII references")
{
    Memory memory;
    Fake fake;
    {
        auto first = fake.create(memory.resource); REQUIRE(first);
        auto second = fake.create(memory.resource); REQUIRE(second);
        const auto* device = &second->logical_device();
        const auto* instance = &second->instance();
        *first = std::move(*second);
        REQUIRE(fake.destroyed == "ADMI");
        REQUIRE(&first->logical_device() == device);
        REQUIRE(&first->instance() == instance);
        REQUIRE(static_cast<VkDevice>(*first->logical_device()) == first->native_device());
    }
    REQUIRE(fake.destroyed == "ADMIADMI");
    REQUIRE(memory.resource.snapshot().live_allocations == 0);
}

TEST_CASE("shared queue views retain one native device until the last owner")
{
    Memory memory; Fake fake; fake.available_queues=2;
    std::optional<Device> worker;
    {
        auto owner=fake.create(memory.resource,{.secondary_queue=true}); REQUIRE(owner);
        REQUIRE(owner->queue_count()==2); CHECK(fake.requested_queues==2);
        auto second=owner->share_queue(1); REQUIRE(second);
        CHECK(second->native_device()==owner->native_device());
        CHECK(second->allocator()==owner->allocator());
        CHECK(*second->queue()!=*owner->queue()); CHECK(second->queue_index()==1);
        CHECK_FALSE(owner->share_queue(2));
        worker.emplace(std::move(*second));
    }
    CHECK(fake.destroyed.empty());
    worker.reset(); CHECK(fake.destroyed=="ADMI");
    CHECK(memory.resource.snapshot().live_allocations==0);
}
TEST_CASE("secondary queue request preserves single queue fallback")
{
    Memory memory; Fake fake;
    {
        auto device=fake.create(memory.resource,{.secondary_queue=true}); REQUIRE(device);
        CHECK(device->queue_count()==1); CHECK(fake.requested_queues==1);
        CHECK_FALSE(device->share_queue(1)); CHECK(device->queue_index()==0);
    }
    CHECK(fake.destroyed=="ADMI");
}

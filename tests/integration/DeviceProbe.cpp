#include <dk/graphics/Device.hpp>
#include <dk/memory/MemorySystem.hpp>
#include <atomic>
#include <cstdio>
#include <cstring>

namespace {
struct Diagnostics { std::atomic<unsigned> errors = 0, warnings = 0, probes = 0; };
void diagnostic(void* data, const dk::graphics::Diagnostic& message) noexcept
{
    auto& state = *static_cast<Diagnostics*>(data);
    if (message.severity == VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) ++state.errors;
    if (message.severity == VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) ++state.warnings;
    if (message.name == "dk.device.probe") ++state.probes;
    if (message.severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
        std::fprintf(stderr, "%.*s: %.*s\n", static_cast<int>(message.name.size()), message.name.data(),
            static_cast<int>(message.message.size()), message.message.data());
}
}
int main(int argc, char** argv)
{
    const bool validation = argc == 2 && std::strcmp(argv[1], "--validation") == 0;
    auto system = dk::memory::MemorySystem::create();
    if (!system) return 1;
    auto resource = system->create_heap({"device-probe", dk::memory::DomainCategory::render});
    if (!resource) return 1;
    Diagnostics diagnostics;
    dk::graphics::DeviceOptions options;
    options.validation = validation ? dk::graphics::ValidationMode::required : dk::graphics::ValidationMode::disabled;
    options.diagnostic_sink = diagnostic;
    options.diagnostic_user_data = &diagnostics;
    for (int round = 0; round < 3; ++round) {
        auto device = dk::graphics::Device::create(*resource, options);
        if (!device) {
            std::fprintf(stderr, "%s\n", device.error().message.c_str());
            for (const auto& context : device.error().context) std::fprintf(stderr, "%s\n", context.c_str());
            // Only unavailable environments skip. Unexpected runtime failures fail.
            if (round == 0 && (device.error().code == dk::ErrorCode::not_found || device.error().code == dk::ErrorCode::not_supported)) return 77;
            return 1;
        }
        const auto& info = device->adapter();
        std::printf("round=%d GPU=%s vendor=%u device=%u API=%u.%u.%u driver=%s (%s) driverRaw=%u queueFamily=%u validation=%s\n",
            round, info.properties.deviceName, info.properties.vendorID, info.properties.deviceID,
            VK_API_VERSION_MAJOR(info.properties.apiVersion), VK_API_VERSION_MINOR(info.properties.apiVersion), VK_API_VERSION_PATCH(info.properties.apiVersion),
            info.driver.driverName, info.driver.driverInfo, info.properties.driverVersion, device->queue_family(), device->validation_enabled() ? "on" : "off");
        if (!device->instance() || !device->physical_device() || !device->native_device() || !device->queue() ||
            !device->device_proc("vkDeviceWaitIdle")) return 1;
        if (validation) {
            auto submit = reinterpret_cast<PFN_vkSubmitDebugUtilsMessageEXT>(device->instance_proc("vkSubmitDebugUtilsMessageEXT"));
            if (!submit) return 1;
            VkDebugUtilsMessengerCallbackDataEXT data{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CALLBACK_DATA_EXT};
            data.pMessageIdName = "dk.device.probe";
            data.pMessage = "Device diagnostic routing probe";
            submit(device->instance(), VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT, VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT, &data);
        }
    }
    // The sink survives native teardown so destruction diagnostics are included.
    std::printf("errors=%u warnings=%u routed=%u liveAllocations=%zu\n", diagnostics.errors.load(), diagnostics.warnings.load(), diagnostics.probes.load(), resource->snapshot().live_allocations);
    if (diagnostics.errors != 0 || (validation && diagnostics.probes != 3) || resource->snapshot().live_allocations != 0) return 1;
    return system->try_close().closed() ? 0 : 1;
}

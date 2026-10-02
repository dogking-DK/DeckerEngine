#include <dk/graphics/PresentDevice.hpp>
#include <dk/memory/MemorySystem.hpp>
#include <cstdio>

int main()
{
    auto system = dk::memory::MemorySystem::create();
    if (!system) return 1;
    auto heap = system->create_heap({"present-device", dk::memory::DomainCategory::render});
    if (!heap) return 1;
    std::uint64_t errors = 0, warnings = 0;
    struct Counts { std::uint64_t& errors; std::uint64_t& warnings; } counts{errors,warnings};
    dk::graphics::DeviceOptions options;
    options.validation = dk::graphics::ValidationMode::required;
    options.diagnostic_user_data = &counts;
    options.diagnostic_sink = [](void* user, const dk::graphics::Diagnostic& message) noexcept {
        auto& value = *static_cast<Counts*>(user);
        if (message.severity == VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) ++value.errors;
        if (message.severity == VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) ++value.warnings;
        if (message.severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
            std::fprintf(stderr, "%.*s\n", static_cast<int>(message.message.size()), message.message.data());
    };
    for (int round = 0; round < 3; ++round) {
        auto window = dk::platform::Window::create(*heap, {"DeckerEngine surface probe",320,240});
        if (!window) { std::fprintf(stderr, "%s\n", window.error().message.c_str()); return 1; }
        auto device = dk::graphics::create_present_device(*heap, *window, options);
        if (!device) {
            std::fprintf(stderr, "%s\n", device.error().message.c_str());
            for (const auto& context : device.error().context) std::fprintf(stderr,"%s\n",context.c_str());
            return device.error().code == dk::ErrorCode::not_supported || device.error().code == dk::ErrorCode::not_found ? 77 : 1;
        }
        *window = {}; // Device must retain window through surface destruction.
        if (!device->surface() || !device->adapter().swapchain_maintenance1 || !device->adapter().present_queues[device->queue_family()]) return 1;
        std::printf("GPU=%s queue=%u maintenance1=1\n", device->adapter().properties.deviceName.data(), device->queue_family());
    }
    std::printf("errors=%llu warnings=%llu liveAllocations=%zu\n", static_cast<unsigned long long>(errors),
        static_cast<unsigned long long>(warnings), heap->snapshot().live_allocations);
    return errors == 0 && warnings == 0 && heap->snapshot().live_allocations == 0 && system->try_close().closed() ? 0 : 1;
}

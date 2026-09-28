#include <dk/graphics/Device.hpp>
#include <limits>

namespace dk::graphics {
Result<bool> select_validation(ValidationMode mode, bool layer, bool debug)
{
    switch (mode) {
    case ValidationMode::disabled: return false;
    case ValidationMode::if_available: return layer && debug;
    case ValidationMode::required:
        if (!layer) return std::unexpected(Error{ErrorCode::not_supported, "VK_LAYER_KHRONOS_validation is unavailable"});
        if (!debug) return std::unexpected(Error{ErrorCode::not_supported, "VK_EXT_debug_utils is unavailable"});
        return true;
    }
    return std::unexpected(Error{ErrorCode::invalid_argument, "invalid validation mode"});
}

Result<AdapterSelection> select_adapter(std::span<const AdapterInfo> adapters, std::optional<std::uint32_t> requested)
{
    if (requested && *requested >= adapters.size())
        return std::unexpected(Error{ErrorCode::not_found, "requested Vulkan adapter index is unavailable"});
    if (adapters.empty()) return std::unexpected(Error{ErrorCode::not_found, "no Vulkan physical devices found"});
    Error failure{ErrorCode::not_supported, "no Vulkan adapter satisfies the device requirements"};
    std::optional<AdapterSelection> selected;
    int best_rank = -1;
    for (std::size_t i = 0; i < adapters.size(); ++i) {
        if (requested && i != *requested) continue;
        const auto& adapter = adapters[i];
        std::string missing;
        const auto require = [&](bool condition, const char* reason) { if (!condition) { missing += reason; missing += "; "; } };
        require(VK_API_VERSION_VARIANT(adapter.properties.apiVersion) == 0 && adapter.properties.apiVersion >= device_api_version, "Vulkan 1.3");
        require(adapter.timeline_semaphore, "timelineSemaphore");
        require(adapter.synchronization2, "synchronization2");
        require(adapter.dynamic_rendering, "dynamicRendering");
        std::optional<std::uint32_t> family;
        for (std::size_t q = 0; q < adapter.queues.size(); ++q) {
            const auto& queue = adapter.queues[q];
            constexpr auto flags = vk::QueueFlagBits::eGraphics | vk::QueueFlagBits::eCompute;
            if (queue.queueCount > 0 && (queue.queueFlags & flags) == flags) {
                family = static_cast<std::uint32_t>(q); break;
            }
        }
        require(family.has_value(), "graphics+compute queue");
        if (!missing.empty()) {
            failure.context.push_back("adapter[" + std::to_string(i) + "] " + adapter.properties.deviceName.data() + ": " + missing);
            continue;
        }
        int rank = 0;
        switch (adapter.properties.deviceType) {
        case vk::PhysicalDeviceType::eDiscreteGpu: rank = 4; break;
        case vk::PhysicalDeviceType::eIntegratedGpu: rank = 3; break;
        case vk::PhysicalDeviceType::eVirtualGpu: rank = 2; break;
        case vk::PhysicalDeviceType::eCpu: rank = 1; break;
        default: break;
        }
        if (rank > best_rank) { selected = AdapterSelection{static_cast<std::uint32_t>(i), *family}; best_rank = rank; }
    }
    if (!selected) return std::unexpected(std::move(failure));
    return *selected;
}

std::string_view vulkan_result_name(VkResult result) noexcept
{
    switch (result) {
#define DK_VK_RESULT(value) case value: return #value
    DK_VK_RESULT(VK_SUCCESS);
    DK_VK_RESULT(VK_INCOMPLETE);
    DK_VK_RESULT(VK_ERROR_OUT_OF_HOST_MEMORY);
    DK_VK_RESULT(VK_ERROR_OUT_OF_DEVICE_MEMORY);
    DK_VK_RESULT(VK_ERROR_INITIALIZATION_FAILED);
    DK_VK_RESULT(VK_ERROR_DEVICE_LOST);
    DK_VK_RESULT(VK_ERROR_MEMORY_MAP_FAILED);
    DK_VK_RESULT(VK_ERROR_LAYER_NOT_PRESENT);
    DK_VK_RESULT(VK_ERROR_EXTENSION_NOT_PRESENT);
    DK_VK_RESULT(VK_ERROR_FEATURE_NOT_PRESENT);
    DK_VK_RESULT(VK_ERROR_INCOMPATIBLE_DRIVER);
    DK_VK_RESULT(VK_ERROR_TOO_MANY_OBJECTS);
    DK_VK_RESULT(VK_ERROR_FORMAT_NOT_SUPPORTED);
    DK_VK_RESULT(VK_ERROR_UNKNOWN);
    DK_VK_RESULT(VK_ERROR_VALIDATION_FAILED_EXT);
#undef DK_VK_RESULT
    default: return "unrecognized VkResult";
    }
}
} // namespace dk::graphics

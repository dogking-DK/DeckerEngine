#include <dk/graphics/Device.hpp>
#include <dk/memory/MemorySystem.hpp>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <optional>

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
bool allocator_smoke(const dk::graphics::Device& device)
{
    if (!device.allocator()) return false;
    struct Buffer {
        VmaAllocator allocator;
        VkBuffer buffer = VK_NULL_HANDLE;
        VmaAllocation allocation = VK_NULL_HANDLE;
        ~Buffer() { if (buffer) vmaDestroyBuffer(allocator, buffer, allocation); }
    };
    {
        Buffer buffer{device.allocator()};
        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        info.size = 4096;
        info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        VmaAllocationCreateInfo allocation{};
        allocation.usage = VMA_MEMORY_USAGE_AUTO;
        allocation.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;
        if (vmaCreateBuffer(buffer.allocator, &info, &allocation, &buffer.buffer, &buffer.allocation, nullptr) != VK_SUCCESS) return false;
        void* mapped = nullptr;
        if (vmaMapMemory(buffer.allocator, buffer.allocation, &mapped) != VK_SUCCESS) return false;
        std::memset(mapped, 0x5a, 4096);
        const auto flushed = vmaFlushAllocation(buffer.allocator, buffer.allocation, 0, VK_WHOLE_SIZE);
        const auto invalidated = vmaInvalidateAllocation(buffer.allocator, buffer.allocation, 0, VK_WHOLE_SIZE);
        bool matched = true;
        for (std::size_t i = 0; i < 4096; ++i) if (static_cast<unsigned char*>(mapped)[i] != 0x5a) matched = false;
        vmaUnmapMemory(buffer.allocator, buffer.allocation);
        if (!matched || flushed != VK_SUCCESS || invalidated != VK_SUCCESS) return false;
    }
    VmaTotalStatistics stats{};
    vmaCalculateStatistics(device.allocator(), &stats);
    return stats.total.statistics.allocationCount == 0;
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
        auto created = dk::graphics::Device::create(*resource, options);
        if (!created) {
            std::fprintf(stderr, "%s\n", created.error().message.c_str());
            for (const auto& context : created.error().context) std::fprintf(stderr, "%s\n", context.c_str());
            // Only unavailable environments skip. Unexpected runtime failures fail.
            if (round == 0 && (created.error().code == dk::ErrorCode::not_found || created.error().code == dk::ErrorCode::not_supported)) return 77;
            return 1;
        }
        std::optional<dk::graphics::Device> device{std::move(*created)};
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
        auto peer = dk::graphics::Device::create(*resource, options);
        if (!peer || peer->instance() == device->instance() || peer->native_device() == device->native_device() ||
            peer->allocator() == device->allocator()) return 1;
        if (!allocator_smoke(*device) || !allocator_smoke(*peer)) return 1;
        device.reset();
        if (!allocator_smoke(*peer)) return 1;
        std::printf("VMA round=%d buffer map/flush/invalidate/free passed; peer survived first device destruction\n", round);
    }
    // The sink survives native teardown so destruction diagnostics are included.
    std::printf("errors=%u warnings=%u routed=%u liveAllocations=%zu\n", diagnostics.errors.load(), diagnostics.warnings.load(), diagnostics.probes.load(), resource->snapshot().live_allocations);
    if (diagnostics.errors != 0 || (validation && diagnostics.probes != 3) || resource->snapshot().live_allocations != 0) return 1;
    return system->try_close().closed() ? 0 : 1;
}

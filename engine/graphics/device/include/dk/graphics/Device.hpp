#pragma once

#include <dk/core/Result.hpp>
#include <dk/memory/Containers.hpp>
#include <dk/memory/SmartPtr.hpp>
#include <vulkan/vulkan_raii.hpp>
#include <vk_mem_alloc.h>

#include <filesystem>
#include <optional>
#include <span>

namespace dk::graphics {

inline constexpr std::uint32_t device_api_version = VK_API_VERSION_1_3;
enum class ValidationMode { disabled, if_available, required };

struct Diagnostic {
    VkDebugUtilsMessageSeverityFlagBitsEXT severity;
    VkDebugUtilsMessageTypeFlagsEXT types;
    std::int32_t id;
    std::string_view name;
    std::string_view message;
};
// Message views are borrowed for the call. Must be thread safe, noexcept, and
// must not reenter device operations. User data outlives creation/destruction.
using DiagnosticSink = void (*)(void*, const Diagnostic&) noexcept;
struct DeviceOptions {
    ValidationMode validation = ValidationMode::if_available;
    std::optional<std::uint32_t> adapter_index;
    std::filesystem::path loader_path; // Empty: system loader; otherwise absolute.
    DiagnosticSink diagnostic_sink = nullptr;
    void* diagnostic_user_data = nullptr;
};

struct AdapterInfo {
    explicit AdapterInfo(memory::ResourceHandle resource) : queues(memory::Allocator<vk::QueueFamilyProperties>{resource}) {}
    vk::PhysicalDeviceProperties properties{};
    vk::PhysicalDeviceDriverProperties driver{};
    bool timeline_semaphore = false;
    bool synchronization2 = false;
    bool dynamic_rendering = false;
    Vector<vk::QueueFamilyProperties> queues;
};
struct AdapterSelection { std::uint32_t adapter_index; std::uint32_t queue_family; };
// Pure policy functions; no loader, instance, or GPU is needed.
[[nodiscard]] Result<AdapterSelection> select_adapter(std::span<const AdapterInfo> adapters,
    std::optional<std::uint32_t> requested = {});
[[nodiscard]] Result<bool> select_validation(ValidationMode mode, bool layer_available, bool debug_utils_available);
[[nodiscard]] std::string_view vulkan_result_name(VkResult result) noexcept;

namespace detail { struct DeviceAccess; }
class Device final {
public:
    [[nodiscard]] static Result<Device> create(memory::ResourceHandle resource, const DeviceOptions& options = {});
    ~Device();
    // A moved-from object can only be destroyed or assigned to.
    Device(Device&&) noexcept;
    Device& operator=(Device&&) noexcept;
    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;

    // Borrowed RAII objects/function pointers. Consumer synchronizes access and
    // destroys children/completes submitted work before this owner is destroyed.
    // Dereference for vk::* handles; never give those handles another owner.
    [[nodiscard]] const vk::raii::Instance& instance() const noexcept;
    [[nodiscard]] const vk::raii::PhysicalDevice& physical_device() const noexcept;
    [[nodiscard]] const vk::raii::Device& logical_device() const noexcept;
    [[nodiscard]] const vk::raii::Queue& queue() const noexcept;
    // Borrowed C handle for interoperability (e.g. VMA).
    [[nodiscard]] VkDevice native_device() const noexcept;
    // Borrowed allocator. Release every allocation before destroying this Device.
    [[nodiscard]] VmaAllocator allocator() const noexcept;
    [[nodiscard]] std::uint32_t queue_family() const noexcept;
    [[nodiscard]] const AdapterInfo& adapter() const noexcept;
    [[nodiscard]] bool validation_enabled() const noexcept;
    [[nodiscard]] std::uint64_t validation_errors() const noexcept;
    [[nodiscard]] std::uint64_t validation_warnings() const noexcept;
    [[nodiscard]] PFN_vkVoidFunction instance_proc(const char* name) const noexcept;
    [[nodiscard]] PFN_vkVoidFunction device_proc(const char* name) const noexcept;
private:
    friend struct detail::DeviceAccess;
    struct Impl;
    explicit Device(memory::UniquePtr<Impl> impl) noexcept;
    memory::UniquePtr<Impl> impl_;
};
} // namespace dk::graphics

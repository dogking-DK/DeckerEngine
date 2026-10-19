#pragma once
#include <mutex>

#include <dk/core/Result.hpp>
#include <dk/memory/Containers.hpp>
#include <dk/memory/SmartPtr.hpp>
#include <vulkan/vulkan_raii.hpp>
#include <vk_mem_alloc.h>

#include <filesystem>
#include <array>
#include <optional>
#include <span>
#include <stop_token>

namespace dk::graphics {

// Runtime API baseline, not the Vulkan header/SDK patch version.
inline constexpr std::uint32_t device_api_version = VK_API_VERSION_1_4;
enum class ValidationMode { disabled, if_available, required };
enum class RayTracingMode { disabled, if_available, required };
enum class SampledImageIndexingMode { disabled, if_available, required };

// Hardware support, independent of logical-device enablement. No borrowed pNext.
struct RayTracingSupport {
    bool acceleration_structure_extension = false, ray_tracing_pipeline_extension = false;
    bool deferred_host_operations = false, buffer_device_address = false;
    bool acceleration_structure = false, ray_tracing_pipeline = false;
    vk::PhysicalDeviceAccelerationStructurePropertiesKHR acceleration_structure_limits{};
    vk::PhysicalDeviceRayTracingPipelinePropertiesKHR pipeline_limits{};
    [[nodiscard]] bool supported() const noexcept;
    // Fixed requirement order; satisfied requirements have empty string views.
    [[nodiscard]] std::array<std::string_view, 6> missing() const noexcept;
};

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
// Optional WSI bridge. Called once after instance creation, before selection.
// Successful callback transfers surface ownership to Device; owner keeps the
// native window alive until after surface/instance destruction. No SDL dependency.
struct SurfaceSource {
    std::span<const char* const> instance_extensions;
    Result<VkSurfaceKHR> (*create)(VkInstance, void*) = nullptr;
    void* user_data = nullptr;
    std::shared_ptr<void> owner;
};
struct DeviceOptions {
    ValidationMode validation = ValidationMode::if_available;
    std::optional<std::uint32_t> adapter_index;
    std::filesystem::path loader_path; // Empty: system loader; otherwise absolute.
    DiagnosticSink diagnostic_sink = nullptr;
    void* diagnostic_user_data = nullptr;
    SurfaceSource surface;
    std::stop_token cancel; // Cooperative checkpoints; never interrupts a driver call.
    bool secondary_queue = false; // Request a second same-family queue when available.
    RayTracingMode ray_tracing = RayTracingMode::disabled;
    SampledImageIndexingMode sampled_image_indexing = SampledImageIndexingMode::disabled;
};

struct AdapterInfo {
    explicit AdapterInfo(memory::ResourceHandle resource) : queues(memory::Allocator<vk::QueueFamilyProperties>{resource}),
        present_queues(memory::Allocator<std::uint8_t>{resource}) {}
    vk::PhysicalDeviceProperties properties{};
    vk::PhysicalDeviceDriverProperties driver{};
    bool timeline_semaphore = false;
    bool synchronization2 = false;
    bool dynamic_rendering = false;
    bool maintenance4 = false;
    Vector<vk::QueueFamilyProperties> queues;
    Vector<std::uint8_t> present_queues;
    bool swapchain = false, swapchain_maintenance1 = false;
    RayTracingSupport ray_tracing;
    bool sampled_image_non_uniform_indexing = false;
    bool memory_budget = false; // Optional VK_EXT_memory_budget, enabled for VMA when supported.
};
struct AdapterSelection { std::uint32_t adapter_index; std::uint32_t queue_family; };
// Pure policy functions; no loader, instance, or GPU is needed.
[[nodiscard]] Result<AdapterSelection> select_adapter(std::span<const AdapterInfo> adapters,
    std::optional<std::uint32_t> requested = {}, bool presentation = false,
    RayTracingMode ray_tracing = RayTracingMode::disabled,
    SampledImageIndexingMode sampled_image_indexing = SampledImageIndexingMode::disabled);
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
    [[nodiscard]] std::uint32_t queue_count() const noexcept;
    [[nodiscard]] std::uint32_t queue_index() const noexcept { return queue_index_; }
    // Shared native-call gate; each SubmissionQueue ledger still needs one serial owner.
    [[nodiscard]] std::mutex& queue_gate() const noexcept;
    // Explicit shared lifetime. Native borrowed queue calls must hold queue_gate().
    // The final view must be released on the surface/window owner's thread, if any.
    [[nodiscard]] Result<Device> share_queue(std::uint32_t index) const;
    [[nodiscard]] vk::SurfaceKHR surface() const noexcept;
    [[nodiscard]] const AdapterInfo& adapter() const noexcept;
    // AS + RT pipeline + BDA are enabled together; queue views share this state.
    [[nodiscard]] bool ray_tracing_enabled() const noexcept;
    [[nodiscard]] bool sampled_image_indexing_enabled() const noexcept;
    [[nodiscard]] bool validation_enabled() const noexcept;
    [[nodiscard]] std::uint64_t validation_errors() const noexcept;
    [[nodiscard]] std::uint64_t validation_warnings() const noexcept;
    [[nodiscard]] PFN_vkVoidFunction instance_proc(const char* name) const noexcept;
    [[nodiscard]] PFN_vkVoidFunction device_proc(const char* name) const noexcept;
private:
    friend struct detail::DeviceAccess;
    struct Impl;
    explicit Device(std::shared_ptr<Impl> impl,std::uint32_t index=0) noexcept;
    std::shared_ptr<Impl> impl_;
    std::uint32_t queue_index_=0;
};
} // namespace dk::graphics

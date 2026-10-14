#pragma once
#include <dk/graphics/AccelerationStructure.hpp>
#include <dk/graphics/CommandEncoder.hpp>

namespace dk::graphics {
namespace detail { struct RayTracingState; struct ShaderTableState; }
struct RayTracingHitGroup { const ShaderModule* closest_hit = nullptr; const ShaderModule* any_hit = nullptr; };
struct RayTracingPipelineDesc {
    const PipelineLayout* layout = nullptr;
    const ShaderModule* raygeneration = nullptr;
    std::span<const ShaderModule* const> misses;
    std::span<const RayTracingHitGroup> hits; // Interleaved [geometry class][ray type].
    std::uint32_t ray_type_count = 2;
};
class RayTracingPipeline final {
public:
    RayTracingPipeline() = default;
    RayTracingPipeline(RayTracingPipeline&&) noexcept = default;
    RayTracingPipeline& operator=(RayTracingPipeline&&) noexcept = default;
    RayTracingPipeline(const RayTracingPipeline&) = delete;
    RayTracingPipeline& operator=(const RayTracingPipeline&) = delete;
    [[nodiscard]] explicit operator bool() const noexcept { return bool(state_); }
    [[nodiscard]] vk::Pipeline handle() const noexcept;
private:
    friend class ResourceFactory;
    friend struct detail::ObjectAccess;
    explicit RayTracingPipeline(std::shared_ptr<detail::RayTracingState> state) : state_(std::move(state)) {}
    std::shared_ptr<detail::RayTracingState> state_;
};
struct ShaderBindingTableRegions {
    vk::StridedDeviceAddressRegionKHR raygeneration, miss, hit, callable;
};
class ShaderBindingTable final {
public:
    ShaderBindingTable() = default;
    ShaderBindingTable(ShaderBindingTable&&) noexcept = default;
    ShaderBindingTable& operator=(ShaderBindingTable&&) noexcept = default;
    ShaderBindingTable(const ShaderBindingTable&) = delete;
    ShaderBindingTable& operator=(const ShaderBindingTable&) = delete;
    [[nodiscard]] explicit operator bool() const noexcept { return bool(state_); }
    [[nodiscard]] ShaderBindingTableRegions regions() const noexcept; // Borrowed, immutable SBT addresses.
private:
    friend class ResourceFactory;
    friend struct detail::ObjectAccess;
    explicit ShaderBindingTable(std::shared_ptr<detail::ShaderTableState> state) : state_(std::move(state)) {}
    std::shared_ptr<detail::ShaderTableState> state_;
};
class RayTracingEncoder final {
public:
    RayTracingEncoder() = default;
    // All AS/SBT and ordinary resource accesses must be prepared explicitly before trace.
    [[nodiscard]] Result<void> trace(const RayTracingPipeline& pipeline, const ShaderBindingTable& table,
        std::span<const BindingSet* const> sets, std::span<const std::byte> push_constants, std::array<std::uint32_t,3> dimensions);
private:
    friend class CommandBatch;
    RayTracingEncoder(const std::shared_ptr<detail::BatchState>& batch, std::uint64_t generation) : batch_(batch), generation_(generation) {}
    std::weak_ptr<detail::BatchState> batch_;
    std::uint64_t generation_ = 0;
};
} // namespace dk::graphics

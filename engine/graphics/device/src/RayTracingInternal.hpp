#pragma once
#include "AccelerationInternal.hpp"

namespace dk::graphics::detail {
inline constexpr auto ray_shader_stages = vk::ShaderStageFlagBits::eRaygenKHR | vk::ShaderStageFlagBits::eMissKHR |
    vk::ShaderStageFlagBits::eClosestHitKHR | vk::ShaderStageFlagBits::eAnyHitKHR | vk::ShaderStageFlagBits::eIntersectionKHR;
struct RayTracingState : PipelineState {
    using PipelineState::PipelineState;
    std::uint32_t ray_types = 0, miss_count = 0, hit_count = 0;
    std::array<RayGeometryKind,120> hit_kinds{};
};
struct ShaderTableState : ObjectState {
    using ObjectState::ObjectState;
    std::shared_ptr<RayTracingState> pipeline;
    Buffer storage;
    ShaderBindingTableRegions regions;
};
struct ShaderTableLayout {
    vk::DeviceSize stride = 0, miss_offset = 0, hit_offset = 0, bytes = 0, capacity = 0;
};
Result<ShaderTableLayout> shader_table_layout(const vk::PhysicalDeviceRayTracingPipelinePropertiesKHR& limits,
    std::uint32_t misses, std::uint32_t hits);
Result<void> ray_dispatch_dimensions(std::array<std::uint32_t,3> dimensions, const vk::PhysicalDeviceLimits& limits,
    std::uint32_t max_invocations);
Result<void> ray_hit_range(std::uint32_t offset, std::uint32_t geometries, std::uint32_t ray_types, std::uint32_t hit_count);
Result<void> ray_hit_kinds(const AccelerationHitRange& range, std::uint32_t ray_types, std::span<const RayGeometryKind> kinds);
Result<void> ray_hit_attributes(RayGeometryKind kind, const ShaderRayInterface* intersection,
    const ShaderRayInterface& closest, const ShaderRayInterface* any, std::uint32_t max_bytes);
Result<void> ray_payload_compatible(const ShaderRayInterface& caller, const ShaderRayInterface& callee, const ShaderRayTrace& trace);
} // namespace dk::graphics::detail

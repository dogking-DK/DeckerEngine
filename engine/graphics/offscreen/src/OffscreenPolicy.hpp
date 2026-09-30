#pragma once
#include <dk/graphics/Offscreen.hpp>

namespace dk::graphics::detail {
[[nodiscard]] Result<void> validate_shader(const CompiledShader& shader, ShaderStage stage);
[[nodiscard]] Result<std::size_t> validate_draw(const CompiledShader& vertex, const CompiledShader& fragment,
    const OffscreenDraw& description, const vk::PhysicalDeviceLimits& limits);
[[nodiscard]] Result<void> validate_dispatch(const CompiledShader& shader, std::span<const ComputeBufferInput> buffers,
    std::span<const std::byte> push_constants, std::array<std::uint32_t, 3> groups, const vk::PhysicalDeviceLimits& limits);
// Private test seam: inject M5.2's driver-facing queue without exposing it publicly.
struct OffscreenAccess {
    [[nodiscard]] static Result<OffscreenExecutor> create(memory::ResourceHandle resource, SubmissionQueue&& queue);
};
} // namespace dk::graphics::detail

#pragma once
#include <dk/core/Result.hpp>
#include <dk/memory/Containers.hpp>
#include <array>
#include <string_view>

namespace dk::graphics {
enum class ShaderStage { vertex, fragment, compute };
enum class ShaderDescriptorType { uniform_buffer, storage_buffer, sampled_image, storage_image, sampler };
[[nodiscard]] std::string_view shader_stage_name(ShaderStage stage) noexcept;
[[nodiscard]] std::string_view shader_descriptor_name(ShaderDescriptorType type) noexcept;
struct ShaderBinding {
    String name;
    std::uint32_t set = 0, binding = 0;
    ShaderDescriptorType type = ShaderDescriptorType::storage_buffer;
    std::uint32_t count = 1, block_size = 0;
};
struct ShaderPushConstant { std::uint32_t offset = 0, size = 0; };
// Owned CPU artifact. No compiler session or Vulkan dependency.
struct CompiledShader {
    explicit CompiledShader(memory::ResourceHandle resource);
    Vector<std::uint32_t> spirv;
    String entry, compiler, diagnostics;
    ShaderStage stage = ShaderStage::compute;
    std::array<std::uint32_t, 3> thread_group_size{};
    Vector<ShaderBinding> bindings;
    Vector<ShaderPushConstant> push_constants;
    Vector<String> dependencies;
};
// Structural baseline check, not a complete SPIR-V validator.
[[nodiscard]] Result<void> validate_shader_artifact(const CompiledShader& shader);
} // namespace dk::graphics

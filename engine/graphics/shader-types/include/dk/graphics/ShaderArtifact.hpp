#pragma once
#include <dk/core/Result.hpp>
#include <dk/memory/Containers.hpp>
#include <array>
#include <string_view>

namespace dk::graphics {
enum class ShaderStage { vertex, fragment, compute, raygeneration, miss, closesthit, anyhit };
[[nodiscard]] bool is_ray_tracing_stage(ShaderStage stage) noexcept;
enum class ShaderImageShape { none, image_2d, image_2d_array, image_3d };
[[nodiscard]] std::string_view shader_image_shape_name(ShaderImageShape shape) noexcept;
enum class ShaderDescriptorType { uniform_buffer, storage_buffer, sampled_image, storage_image, sampler, acceleration_structure };
[[nodiscard]] std::string_view shader_stage_name(ShaderStage stage) noexcept;
[[nodiscard]] std::string_view shader_descriptor_name(ShaderDescriptorType type) noexcept;
struct ShaderBinding {
    String name;
    std::uint32_t set = 0, binding = 0;
    ShaderDescriptorType type = ShaderDescriptorType::storage_buffer;
    std::uint32_t count = 1, block_size = 0;
    ShaderImageShape image_shape = ShaderImageShape::none;
};
struct ShaderPushConstant { std::uint32_t offset = 0, size = 0; };
enum class RayInterfaceKind { outgoing_payload, incoming_payload, hit_attribute };
struct ShaderRayPayload {
    RayInterfaceKind kind{};
    std::uint32_t location = 0, bytes = 0;
    String signature; // Canonical structural type, independent of SPIR-V ids/names.
    bool operator==(const ShaderRayPayload&) const = default;
};
struct ShaderRayTrace {
    std::uint32_t offset = 0, stride = 0, miss = 0, payload_location = 0;
    bool operator==(const ShaderRayTrace&) const = default;
};
struct ShaderRayInterface {
    explicit ShaderRayInterface(memory::ResourceHandle resource)
        : payloads(memory::Allocator<ShaderRayPayload>{resource}), traces(memory::Allocator<ShaderRayTrace>{resource}) {}
    Vector<ShaderRayPayload> payloads;
    Vector<ShaderRayTrace> traces;
    bool operator==(const ShaderRayInterface&) const = default;
};
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
    bool requires_ray_tracing = false;
    bool requires_sampled_image_indexing = false;
    std::uint64_t source_fingerprint = 0; // FNV-1a content/config fingerprint; not a security digest.
    ShaderRayInterface ray_interface;
};
// Bounded supported-interface extraction, not a complete SPIR-V validator.
[[nodiscard]] Result<ShaderRayInterface> reflect_ray_interface(const CompiledShader& shader);
// Structural baseline check, not a complete SPIR-V validator.
[[nodiscard]] Result<void> validate_shader_artifact(const CompiledShader& shader);
} // namespace dk::graphics

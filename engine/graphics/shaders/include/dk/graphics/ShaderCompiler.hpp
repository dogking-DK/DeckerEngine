#pragma once

#include <dk/core/Result.hpp>
#include <dk/memory/Containers.hpp>
#include <array>
#include <filesystem>
#include <span>
#include <string_view>

namespace dk::graphics {

enum class ShaderStage { vertex, fragment, compute };
enum class ShaderDescriptorType { uniform_buffer, storage_buffer, sampled_image, storage_image, sampler };
[[nodiscard]] std::string_view shader_stage_name(ShaderStage stage) noexcept;
[[nodiscard]] std::string_view shader_descriptor_name(ShaderDescriptorType type) noexcept;

struct ShaderDefine { std::string_view name; std::string_view value; };
struct ShaderCompileRequest {
    std::filesystem::path source;
    std::string_view entry;
    ShaderStage stage = ShaderStage::compute;
    std::span<const std::filesystem::path> include_paths{};
    std::span<const ShaderDefine> defines{};
};

struct ShaderBinding {
    String name;
    std::uint32_t set = 0;
    std::uint32_t binding = 0;
    ShaderDescriptorType type = ShaderDescriptorType::storage_buffer;
    std::uint32_t count = 1;
    std::uint32_t block_size = 0; // Uniform buffer bytes; zero for other descriptors.
};
struct ShaderPushConstant { std::uint32_t offset = 0; std::uint32_t size = 0; };

// Owns its Memory resource through every container allocator. No borrowed Slang objects.
struct CompiledShader {
    explicit CompiledShader(memory::ResourceHandle resource);
    Vector<std::uint32_t> spirv;
    String entry;
    String compiler;
    String diagnostics;
    ShaderStage stage = ShaderStage::compute;
    std::array<std::uint32_t, 3> thread_group_size{}; // Zero for graphics stages.
    Vector<ShaderBinding> bindings;
    Vector<ShaderPushConstant> push_constants;
    Vector<String> dependencies; // UTF-8 source/include/import paths, sorted and unique.
};

// Fresh Slang session per call. Input files must remain stable during the call.
// Requires a live owning heap resource. Allocation failures throw std::bad_alloc.
[[nodiscard]] Result<CompiledShader> compile_shader(const ShaderCompileRequest& request,
                                                  memory::ResourceHandle resource);
// Stable schema v1; excludes source/output absolute paths and diagnostic prose.
[[nodiscard]] String shader_reflection_json(const CompiledShader& shader);

} // namespace dk::graphics

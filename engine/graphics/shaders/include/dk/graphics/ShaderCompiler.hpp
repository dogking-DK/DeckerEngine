#pragma once

#include <dk/graphics/ShaderArtifact.hpp>
#include <dk/memory/Containers.hpp>
#include <array>
#include <filesystem>
#include <span>
#include <string_view>
#include <stop_token>

namespace dk::graphics {

struct ShaderDefine { std::string_view name; std::string_view value; };
struct ShaderCompileRequest {
    std::filesystem::path source;
    std::string_view entry;
    ShaderStage stage = ShaderStage::compute;
    std::span<const std::filesystem::path> include_paths{};
    std::span<const ShaderDefine> defines{};
};

// Fresh Slang session per call. Input files must remain stable during the call.
// Requires a live owning heap resource. Allocation failures throw std::bad_alloc.
[[nodiscard]] Result<CompiledShader> compile_shader(const ShaderCompileRequest& request,
                                                  memory::ResourceHandle resource,std::stop_token cancel={});
// One scoped global session, fresh per-entry sessions; no partial batch is published.
[[nodiscard]] Result<Vector<CompiledShader>> compile_shaders(std::span<const ShaderCompileRequest> requests,
    memory::ResourceHandle resource,std::stop_token cancel={});
// Stable schema v1; excludes source/output absolute paths and diagnostic prose.
[[nodiscard]] String shader_reflection_json(const CompiledShader& shader);

} // namespace dk::graphics

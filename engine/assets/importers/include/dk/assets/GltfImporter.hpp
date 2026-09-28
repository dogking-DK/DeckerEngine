#pragma once
#include <dk/assets/CpuData.hpp>
#include <dk/io/Path.hpp>
#include <span>
#include <stop_token>

namespace dk {
// Callers may lower, but not raise, these hard limits. All byte counts are aggregate.
struct ImportLimits {
    std::size_t source_bytes = 16 * 1024 * 1024, dependency_bytes = 64 * 1024 * 1024;
    std::size_t input_bytes = 128 * 1024 * 1024, output_bytes = 256 * 1024 * 1024;
    std::size_t vertices = 1000000, indices = 3000000, primitives = 4096;
    std::size_t image_dimension = 8192, image_bytes = 64 * 1024 * 1024, texture_bytes = 128 * 1024 * 1024;
};
struct GltfImportRequest {
    std::string_view source;
    std::span<const OutputIdentity> identities;
    double unit_scale = 1;
    ImportLimits limits;
    std::stop_token stop;
};
// Pure candidate: no writes or changes to existing metadata, Project, Scene or published data.
// Requires a bound persistent memory context with scratch; ContextError/bad_alloc propagate.
[[nodiscard]] Result<ImportResult> import_gltf(const ProjectPaths& paths, const GltfImportRequest& request);
} // namespace dk

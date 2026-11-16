#pragma once
#include <dk/assets/CpuData.hpp>
#include <dk/core/Result.hpp>
#include <dk/io/File.hpp>
#include <stop_token>

namespace dk::assets {
struct MeshVoxelizeOptions {
    unsigned resolution = 128; // Longest mesh-local axis, cubic cells; 16..512.
    std::uint64_t maximum_candidates = 16 * 1024 * 1024;
    std::uint64_t maximum_tree_bytes = 256 * 1024 * 1024;
    bool texture_colors = true;
    std::stop_token stop;
};
struct MeshVoxelization {
    ByteBuffer vdb;
    std::string description_json;
    std::string statistics_json;
};
// Surface shell, not solid interior filling. Owns all output; no files/state mutated.
// Input follows import_gltf's single-mesh/local-space contract (unit_scale already applied).
[[nodiscard]] Result<MeshVoxelization> voxelize_mesh(const CpuAsset&, const MeshVoxelizeOptions& = {});
} // namespace dk::assets

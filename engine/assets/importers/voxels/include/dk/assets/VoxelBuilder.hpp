#pragma once

#include <dk/geometry/SparseVoxels.hpp>

namespace dk::assets {
// Programmatic source blocks. Dimensions 1/8/128/4096, aligned to their size;
// later entries replace earlier values. active is source metadata, NOT occupancy.
struct VoxelBlock {
    geometry::voxels::Coord minimum{};
    std::uint32_t dimension = 1;
    std::int32_t value = 0;
    bool active = true;
};
struct VoxelBuildLimits {
    std::uint64_t maximum_blocks = 1024 * 1024;
    std::uint64_t maximum_tree_bytes = 256 * 1024 * 1024;
    std::uint64_t maximum_nano_bytes = 256 * 1024 * 1024;
    std::uint64_t maximum_value_visits = 16 * 1024 * 1024;
};
struct VoxelBuildOptions {
    std::uint32_t palette_size = 1;
    // VDB sample centers (integer coordinates) to asset-local space, not corner coordinates.
    Transformd sample_to_local;
    std::optional<geometry::voxels::Domain> domain;
    VoxelBuildLimits limits;
};
// Atomic owning result; neither input nor previously returned snapshots are modified on failure.
[[nodiscard]] Result<geometry::voxels::SparseGrid> build_voxel_grid(std::span<const VoxelBlock>, const VoxelBuildOptions& = {});
} // namespace dk::assets

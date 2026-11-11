#pragma once
// Private bridge for M23.5's future file decoder and M23.2's source-contract tests.
#include <dk/assets/VoxelBuilder.hpp>
#include <openvdb/openvdb.h>

namespace dk::assets::detail {
[[nodiscard]] Result<geometry::voxels::SparseGrid> convert_openvdb(const openvdb::GridBase&, const VoxelBuildOptions&);
}

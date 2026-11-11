#pragma once
#include <dk/assets/CpuData.hpp>
#include <dk/assets/VoxelBuilder.hpp>
#include <dk/io/Path.hpp>
#include <stop_token>

namespace dk::assets {
struct VoxelImportLimits {
    std::uint64_t description_bytes=2*1024*1024, file_bytes=64*1024*1024;
    std::uint64_t input_bytes=128*1024*1024;
    std::uint64_t decoded_tree_bytes=256*1024*1024, live_payload_bytes=896*1024*1024;
    VoxelBuildLimits build;
};
struct VoxelImportRequest {
    std::string_view source;
    std::span<const OutputIdentity> identities;
    double unit_scale=1;
    VoxelImportLimits limits;
    std::stop_token stop;
};
// Read-only, owning input snapshot; requires an open bound persistent memory context.
[[nodiscard]] Result<ImportResult> import_voxel(const ProjectPaths&,const VoxelImportRequest&);
[[nodiscard]] std::string_view voxel_builder_signature() noexcept;
} // namespace dk::assets

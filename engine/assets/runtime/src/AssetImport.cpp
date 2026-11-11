#include "AssetImport.hpp"
#ifdef DK_ASSET_VOXELS
#include <dk/assets/VoxelImporter.hpp>
#endif
namespace dk::asset_detail {
Result<ImportResult> import_asset(const ProjectPaths& paths,const GltfImportRequest& request) {
    if(request.source.ends_with(".dkvoxel")) {
#ifdef DK_ASSET_VOXELS
        assets::VoxelImportRequest v{request.source,request.identities,request.unit_scale,{},request.stop};
        v.limits.description_bytes=std::min<std::uint64_t>(v.limits.description_bytes,request.limits.source_bytes);
        v.limits.file_bytes=std::min<std::uint64_t>(v.limits.file_bytes,request.limits.dependency_bytes);
        v.limits.input_bytes=std::min<std::uint64_t>(v.limits.input_bytes,request.limits.input_bytes);
        v.limits.build.maximum_nano_bytes=std::min<std::uint64_t>(v.limits.build.maximum_nano_bytes,request.limits.output_bytes);
        return assets::import_voxel(paths,v);
#else
        return std::unexpected(Error{ErrorCode::not_supported,"Voxel import requires DK_BUILD_VOXEL_VDB"});
#endif
    }
    return import_gltf(paths,request);
}
}

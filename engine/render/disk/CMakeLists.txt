if(NOT TARGET dk::render_data OR NOT TARGET dk::render_resources OR NOT TARGET dk::asset_importers OR NOT TARGET dk::asset_runtime)
    message(FATAL_ERROR "DK_BUILD_RENDER_DISK requires Render Data/Resources, Asset Importers and Asset Runtime")
endif()
add_library(dk_render_disk STATIC)
add_library(dk::render_disk ALIAS dk_render_disk)
target_sources(dk_render_disk PRIVATE src/DiskScene.cpp
    PUBLIC FILE_SET HEADERS BASE_DIRS include FILES include/dk/render/DiskScene.hpp)
target_link_libraries(dk_render_disk PUBLIC dk::render_data dk::render_resources dk::asset_importers
    PRIVATE dk::asset_runtime dk::io dk::memory dk::profiling)
dk_target_defaults(dk_render_disk)
if(TARGET dk_asset_voxel_importers AND TARGET dk_render_voxels)
    target_link_libraries(dk_render_disk PRIVATE dk::asset_voxel_importers dk::geometry_voxels_sparse)
    target_compile_definitions(dk_render_disk PUBLIC DK_DISK_VOXELS=1)
endif()

if(NOT TARGET dk::asset_data OR NOT TARGET dk::graphics_graph)
    message(FATAL_ERROR "DK_BUILD_RENDER_RESOURCES requires asset data and Graphics Graph")
endif()
add_library(dk_render_resources STATIC)
add_library(dk::render_resources ALIAS dk_render_resources)
target_sources(dk_render_resources PRIVATE src/GpuAssets.cpp src/Validation.cpp
    PUBLIC FILE_SET HEADERS BASE_DIRS include FILES include/dk/render/GpuAssets.hpp)
target_link_libraries(dk_render_resources PUBLIC dk::asset_data dk::graphics_device PRIVATE dk::graphics_graph)
dk_target_defaults(dk_render_resources)

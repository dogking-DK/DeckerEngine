if(NOT TARGET dk::physics_xpbd_gpu)
    message(FATAL_ERROR "DK_BUILD_RENDER_SIMULATION requires GPU Physics")
endif()
add_library(dk_render_simulation STATIC)
add_library(dk::render_simulation ALIAS dk_render_simulation)
target_sources(dk_render_simulation PRIVATE src/ClothRenderer.cpp
    PUBLIC FILE_SET HEADERS BASE_DIRS include FILES include/dk/render/ClothRenderer.hpp)
target_link_libraries(dk_render_simulation PUBLIC dk::physics_xpbd_gpu PRIVATE dk::graphics_shaders)
dk_target_defaults(dk_render_simulation)

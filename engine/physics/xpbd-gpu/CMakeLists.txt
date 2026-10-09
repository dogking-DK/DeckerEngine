if(NOT TARGET dk::graphics_graph OR NOT TARGET dk::graphics_shaders)
    message(FATAL_ERROR "DK_BUILD_PHYSICS_GPU requires Graph and Shaders")
endif()
add_library(dk_physics_xpbd_gpu STATIC)
add_library(dk::physics_xpbd_gpu ALIAS dk_physics_xpbd_gpu)
target_sources(dk_physics_xpbd_gpu PRIVATE src/GpuXpbd.cpp
    PUBLIC FILE_SET HEADERS BASE_DIRS include FILES include/dk/physics/GpuXpbd.hpp)
target_link_libraries(dk_physics_xpbd_gpu PUBLIC dk::physics_xpbd dk::graphics_graph PRIVATE dk::graphics_shaders)
dk_target_defaults(dk_physics_xpbd_gpu)

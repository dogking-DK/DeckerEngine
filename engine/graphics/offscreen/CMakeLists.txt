if(NOT TARGET dk_graphics_device OR NOT TARGET dk_graphics_shaders OR NOT TARGET dk_graphics_graph)
    message(FATAL_ERROR "DK_BUILD_GRAPHICS_OFFSCREEN requires DK_BUILD_GRAPHICS_DEVICE, DK_BUILD_GRAPHICS_SHADERS and DK_BUILD_GRAPHICS_GRAPH")
endif()
add_library(dk_graphics_offscreen STATIC src/Offscreen.cpp src/OffscreenPolicy.cpp)
add_library(dk::graphics_offscreen ALIAS dk_graphics_offscreen)
target_sources(dk_graphics_offscreen PUBLIC FILE_SET HEADERS BASE_DIRS include FILES
    include/dk/graphics/Offscreen.hpp)
target_link_libraries(dk_graphics_offscreen PUBLIC dk::graphics_device dk::graphics_shaders PRIVATE dk::graphics_graph dk::profiling)
dk_target_defaults(dk_graphics_offscreen)

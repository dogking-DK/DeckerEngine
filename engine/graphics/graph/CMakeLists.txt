if(NOT DK_BUILD_GRAPHICS_DEVICE)
    message(FATAL_ERROR "DK_BUILD_GRAPHICS_GRAPH requires DK_BUILD_GRAPHICS_DEVICE")
endif()
add_library(dk_graphics_graph STATIC src/Graph.cpp src/GraphCompile.cpp src/GraphExecution.cpp)
add_library(dk::graphics_graph ALIAS dk_graphics_graph)
target_sources(dk_graphics_graph PUBLIC FILE_SET HEADERS BASE_DIRS include FILES
    include/dk/graphics/Graph.hpp include/dk/graphics/GraphExecution.hpp)
target_link_libraries(dk_graphics_graph PUBLIC dk::graphics_device)
dk_target_defaults(dk_graphics_graph)

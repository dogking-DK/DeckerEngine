if(NOT TARGET dk::scene OR NOT TARGET dk::memory)
    message(FATAL_ERROR "DK_BUILD_RENDER_DATA requires Scene and Memory")
endif()
add_library(dk_render_data STATIC)
add_library(dk::render_data ALIAS dk_render_data)
target_sources(dk_render_data PRIVATE src/RenderScene.cpp
    PUBLIC FILE_SET HEADERS BASE_DIRS include FILES include/dk/render/RenderScene.hpp)
target_link_libraries(dk_render_data PUBLIC dk::scene dk::memory)
dk_target_defaults(dk_render_data)

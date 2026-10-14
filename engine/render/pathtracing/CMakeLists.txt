if(NOT TARGET dk::render_data OR NOT TARGET dk::asset_data)
    message(FATAL_ERROR "Path tracing reference requires RenderData, Scene, Math and Memory")
endif()
add_library(dk_render_pathtracing_reference STATIC src/PathTraceScene.cpp src/PathTraceReference.cpp)
add_library(dk::render_pathtracing_reference ALIAS dk_render_pathtracing_reference)
target_sources(dk_render_pathtracing_reference PUBLIC FILE_SET HEADERS BASE_DIRS include
    FILES include/dk/render/PathTraceReference.hpp)
target_link_libraries(dk_render_pathtracing_reference PUBLIC dk::render_data dk::asset_data)
dk_target_defaults(dk_render_pathtracing_reference)

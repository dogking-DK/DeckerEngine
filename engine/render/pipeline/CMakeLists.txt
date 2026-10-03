if(NOT TARGET dk::render_data OR NOT TARGET dk::render_resources OR NOT TARGET dk::graphics_shaders OR NOT TARGET dk::graphics_graph)
    message(FATAL_ERROR "DK_BUILD_RENDER_PIPELINE requires Render Data, Render Resources, Shaders and Graph")
endif()
add_library(dk_render_pipeline STATIC)
add_library(dk::render_pipeline ALIAS dk_render_pipeline)
target_sources(dk_render_pipeline PRIVATE src/ScenePipeline.cpp src/PipelinePolicy.hpp
    PUBLIC FILE_SET HEADERS BASE_DIRS include FILES include/dk/render/ScenePipeline.hpp)
target_link_libraries(dk_render_pipeline PUBLIC dk::render_data dk::render_resources
    PRIVATE dk::graphics_graph dk::graphics_shaders dk::profiling)
dk_target_defaults(dk_render_pipeline)

add_executable(dk_render_demo src/main.cpp)
set_target_properties(dk_render_demo PROPERTIES OUTPUT_NAME dk-render-demo)
target_link_libraries(dk_render_demo PRIVATE dk::render_disk dk::render_pipeline)
target_compile_definitions(dk_render_demo PRIVATE DK_DEMO_PROJECT_DIR="${PROJECT_SOURCE_DIR}/projects/demo"
    DK_RENDER_SHADER_DIR="${PROJECT_SOURCE_DIR}/shaders/render")
dk_target_defaults(dk_render_demo)
dk_deploy_slang(dk_render_demo)

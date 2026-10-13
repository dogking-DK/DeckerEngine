add_executable(dk_sunlight_demo src/main.cpp)
set_target_properties(dk_sunlight_demo PROPERTIES OUTPUT_NAME dk-sunlight-demo)
target_link_libraries(dk_sunlight_demo PRIVATE dk::render_pipeline dk::render_atmosphere dk::io)
target_compile_definitions(dk_sunlight_demo PRIVATE DK_RENDER_SHADER_DIR="${PROJECT_SOURCE_DIR}/shaders/render"
    DK_ATMOSPHERE_SHADER_DIR="${PROJECT_SOURCE_DIR}/engine/render/atmosphere/shaders")
dk_target_defaults(dk_sunlight_demo)
dk_deploy_slang(dk_sunlight_demo)

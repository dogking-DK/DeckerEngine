add_executable(dk_xpbd_demo src/main.cpp)
set_target_properties(dk_xpbd_demo PROPERTIES OUTPUT_NAME dk-xpbd-demo)
target_link_libraries(dk_xpbd_demo PRIVATE dk::render_simulation)
target_compile_definitions(dk_xpbd_demo PRIVATE DK_XPBD_SHADER="${PROJECT_SOURCE_DIR}/shaders/physics/xpbd.slang"
    DK_CLOTH_SHADER="${PROJECT_SOURCE_DIR}/shaders/render/cloth.slang")
dk_target_defaults(dk_xpbd_demo)
dk_deploy_slang(dk_xpbd_demo)

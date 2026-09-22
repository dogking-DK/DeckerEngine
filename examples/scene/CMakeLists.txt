add_executable(dk_scene_demo src/main.cpp)
set_target_properties(dk_scene_demo PROPERTIES OUTPUT_NAME dk-scene-demo)
target_link_libraries(dk_scene_demo PRIVATE dk::scene)
dk_target_defaults(dk_scene_demo)

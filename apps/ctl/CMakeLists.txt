add_executable(dk_ctl src/main.cpp)
set_target_properties(dk_ctl PROPERTIES OUTPUT_NAME dk-ctl)
target_link_libraries(dk_ctl PRIVATE dk::automation_client)
dk_target_defaults(dk_ctl)

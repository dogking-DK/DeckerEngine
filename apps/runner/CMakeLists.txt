add_executable(dk_run src/main.cpp)
set_target_properties(dk_run PROPERTIES OUTPUT_NAME "dk-run")
target_link_libraries(dk_run PRIVATE dk::core)
dk_target_defaults(dk_run)


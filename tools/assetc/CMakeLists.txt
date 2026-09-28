add_executable(dk_assetc src/main.cpp)
set_target_properties(dk_assetc PROPERTIES OUTPUT_NAME dk-assetc)
target_link_libraries(dk_assetc PRIVATE dk::asset_runtime)
dk_target_defaults(dk_assetc)

find_package(fastgltf CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
add_library(dk_asset_importers STATIC src/GltfImporter.cpp)
add_library(dk::asset_importers ALIAS dk_asset_importers)
target_sources(dk_asset_importers PUBLIC FILE_SET HEADERS BASE_DIRS include FILES include/dk/assets/GltfImporter.hpp)
target_link_libraries(dk_asset_importers PUBLIC dk::asset_data dk::io
    PRIVATE fastgltf::fastgltf nlohmann_json::nlohmann_json dk::profiling)
dk_target_defaults(dk_asset_importers)

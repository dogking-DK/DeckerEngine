if(NOT TARGET dk::io OR NOT TARGET dk::memory)
    message(FATAL_ERROR "DK_BUILD_ASSET_RUNTIME requires DK_BUILD_IO=ON and DK_BUILD_MEMORY=ON")
endif()
find_package(nlohmann_json CONFIG REQUIRED)
add_library(dk_asset_runtime STATIC)
add_library(dk::asset_runtime ALIAS dk_asset_runtime)
target_sources(dk_asset_runtime PRIVATE src/Metadata.cpp src/Catalog.cpp
    PUBLIC FILE_SET HEADERS BASE_DIRS include FILES
        include/dk/assets/Metadata.hpp include/dk/assets/Catalog.hpp)
target_link_libraries(dk_asset_runtime PUBLIC dk::asset_types dk::io dk::memory PRIVATE nlohmann_json::nlohmann_json)
dk_target_defaults(dk_asset_runtime)

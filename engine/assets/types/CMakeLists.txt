add_library(dk_asset_types STATIC)
add_library(dk::asset_types ALIAS dk_asset_types)
target_sources(dk_asset_types PRIVATE src/AssetReference.cpp
    PUBLIC FILE_SET HEADERS BASE_DIRS include FILES include/dk/assets/AssetReference.hpp)
target_link_libraries(dk_asset_types PUBLIC dk::core)
dk_target_defaults(dk_asset_types)

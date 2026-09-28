if(NOT TARGET dk::io OR NOT TARGET dk::memory)
    message(FATAL_ERROR "DK_BUILD_ASSET_RUNTIME requires DK_BUILD_IO=ON and DK_BUILD_MEMORY=ON")
endif()
find_package(nlohmann_json CONFIG REQUIRED)
find_package(xxHash CONFIG REQUIRED)
add_library(dk_asset_runtime STATIC)
add_library(dk::asset_runtime ALIAS dk_asset_runtime)
target_sources(dk_asset_runtime PRIVATE src/Metadata.cpp src/Catalog.cpp src/ContentDigest.cpp src/AssetPersistence.cpp
    PUBLIC FILE_SET HEADERS BASE_DIRS include FILES
        include/dk/assets/Metadata.hpp include/dk/assets/Catalog.hpp include/dk/assets/Persistence.hpp)
target_link_libraries(dk_asset_runtime PUBLIC dk::asset_types dk::io dk::memory PRIVATE dk::profiling nlohmann_json::nlohmann_json xxHash::xxhash)
dk_target_defaults(dk_asset_runtime)
if(TARGET dk_asset_importers)
    target_sources(dk_asset_runtime PRIVATE src/CpuArtifact.cpp src/AssetCompiler.cpp src/CacheFormat.cpp src/AssetCache.cpp src/CacheClean.cpp
        PUBLIC FILE_SET HEADERS BASE_DIRS include FILES include/dk/assets/CpuArtifact.hpp include/dk/assets/AssetCompiler.hpp include/dk/assets/AssetCache.hpp)
    target_link_libraries(dk_asset_runtime PUBLIC dk::asset_importers)
    if(TARGET dk_jobs)
        target_sources(dk_asset_runtime PRIVATE src/AsyncAssets.cpp
            PUBLIC FILE_SET HEADERS BASE_DIRS include FILES include/dk/assets/AsyncAssets.hpp)
        target_link_libraries(dk_asset_runtime PUBLIC dk::jobs)
    endif()
endif()

set(CMAKE_FOLDER "Tools")

if(TARGET dk_asset_runtime AND TARGET dk_asset_importers)
    add_subdirectory(assetc)
endif()
# shaderc remains reserved.

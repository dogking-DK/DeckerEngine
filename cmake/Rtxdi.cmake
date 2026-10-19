# Shader-only dependency. Keep the upstream source/license in the build tree;
# CPU-only configurations neither download nor expose SDK types.
include(FetchContent)
FetchContent_Declare(dk_rtxdi_source
    URL https://codeload.github.com/NVIDIA-RTX/RTXDI-Library/zip/f12037fa8e97ebc08e9e3edfd2de528ed1772a4b
    URL_HASH SHA256=f91de92d7c27f9824915ee5fcf62b0da664a6eb294edb659056ff3c462823f75
    SOURCE_SUBDIR dk-shader-only-no-cmake
    DOWNLOAD_EXTRACT_TIMESTAMP FALSE)
FetchContent_MakeAvailable(dk_rtxdi_source)
add_library(dk_rtxdi_shaders INTERFACE)
add_library(dk::rtxdi_shaders ALIAS dk_rtxdi_shaders)
target_compile_definitions(dk_rtxdi_shaders INTERFACE
    DK_RTXDI_INCLUDE_DIR="${dk_rtxdi_source_SOURCE_DIR}/Include")

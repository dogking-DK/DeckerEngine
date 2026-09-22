add_library(dk_core STATIC)
add_library(dk::core ALIAS dk_core)

target_sources(dk_core
    PRIVATE src/Version.cpp
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS include
        FILES include/dk/core/Version.hpp
)
configure_file(
    "${PROJECT_SOURCE_DIR}/cmake/VersionConfig.hpp.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/dk/core/VersionConfig.hpp"
    @ONLY
)
target_include_directories(dk_core PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
dk_target_defaults(dk_core)


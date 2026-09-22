find_package(stduuid CONFIG REQUIRED)

add_library(dk_core STATIC)
add_library(dk::core ALIAS dk_core)

target_sources(dk_core
    PRIVATE src/Version.cpp src/Error.cpp src/StableId.cpp
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS include
        FILES
            include/dk/core/Version.hpp
            include/dk/core/Error.hpp
            include/dk/core/Result.hpp
            include/dk/core/StableId.hpp
)
configure_file(
    "${PROJECT_SOURCE_DIR}/cmake/VersionConfig.hpp.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/dk/core/VersionConfig.hpp"
    @ONLY
)
target_include_directories(dk_core PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
dk_target_defaults(dk_core)

target_link_libraries(dk_core PRIVATE stduuid)

if(DK_BUILD_LOGGING)
    find_package(fmt CONFIG REQUIRED)
    find_package(spdlog CONFIG REQUIRED)
    add_library(dk_logging STATIC)
    add_library(dk::logging ALIAS dk_logging)
    target_sources(dk_logging
        PRIVATE src/Log.cpp
        PUBLIC FILE_SET HEADERS BASE_DIRS include FILES include/dk/core/Log.hpp
    )
    target_link_libraries(dk_logging
        PUBLIC dk::core fmt::fmt
        PRIVATE spdlog::spdlog
    )
    dk_target_defaults(dk_logging)
endif()

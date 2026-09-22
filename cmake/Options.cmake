include_guard(GLOBAL)

option(DK_BUILD_RUNNER "Build the dk-run bootstrap executable" ON)
option(DK_BUILD_TESTS "Enable available DeckerEngine tests" ON)
option(DK_BUILD_UNIT_TESTS "Build Catch2 unit tests when DK_BUILD_TESTS is enabled" ON)
option(DK_BUILD_LOGGING "Build the fmt/spdlog logging adapter" ON)
option(DK_BUILD_MATH "Build the Eigen math foundation" ON)
option(DK_BUILD_IO "Build project paths and binary file IO" ON)
option(DK_WARNINGS_AS_ERRORS "Treat DeckerEngine warnings as errors" OFF)
option(DK_USE_VCPKG "Use the vcpkg manifest toolchain" ON)
set(DK_VCPKG_FEATURES "foundation" CACHE STRING
    "Extra dependency groups to prepare; required groups are added by enabled targets")

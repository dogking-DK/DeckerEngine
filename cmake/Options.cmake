include_guard(GLOBAL)

option(DK_BUILD_RUNNER "Build the dk-run bootstrap executable" ON)
option(DK_BUILD_TESTS "Enable available DeckerEngine tests" ON)
option(DK_WARNINGS_AS_ERRORS "Treat DeckerEngine warnings as errors" OFF)
option(DK_USE_VCPKG "Use the vcpkg manifest toolchain" ON)
set(DK_VCPKG_FEATURES "foundation" CACHE STRING
    "Dependency groups to prepare (semicolon-separated); these do not implement modules")


# Unit tests do not depend on building the runner.
if(DK_BUILD_UNIT_TESTS)
    add_subdirectory(unit)
endif()

if(TARGET dk_run)
    add_subdirectory(integration)
else()
    message(STATUS "dk-run is disabled; no bootstrap tests are registered.")
endif()

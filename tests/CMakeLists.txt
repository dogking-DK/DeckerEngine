# Keep bootstrap validation independent of future Catch2-based unit tests.
if(TARGET dk_run)
    add_subdirectory(integration)
else()
    message(STATUS "dk-run is disabled; no bootstrap tests are registered.")
endif()


set(CMAKE_FOLDER "Tests")

# Unit tests do not depend on building the runner.
if(DK_BUILD_UNIT_TESTS)
    add_subdirectory(unit)
endif()

add_subdirectory(integration)

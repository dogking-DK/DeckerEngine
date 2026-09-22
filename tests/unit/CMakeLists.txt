find_package(Catch2 3 CONFIG REQUIRED)
find_package(Threads REQUIRED)
find_package(stduuid CONFIG REQUIRED)

add_executable(dk_core_tests ErrorTests.cpp StableIdTests.cpp)
target_link_libraries(dk_core_tests PRIVATE dk::core Catch2::Catch2WithMain Threads::Threads stduuid)
if(DK_BUILD_LOGGING)
    target_sources(dk_core_tests PRIVATE LogTests.cpp)
    target_link_libraries(dk_core_tests PRIVATE dk::logging)
endif()
dk_target_defaults(dk_core_tests)

include(Catch)
if(DK_BUILD_IO)
    add_executable(dk_io_tests IoTests.cpp)
    target_link_libraries(dk_io_tests PRIVATE dk::io Catch2::Catch2WithMain)
    target_compile_definitions(dk_io_tests PRIVATE DK_TEST_CONFIG="$<CONFIG>")
    dk_target_defaults(dk_io_tests)
    catch_discover_tests(dk_io_tests
        TEST_PREFIX "dk.io."
        WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
        DISCOVERY_MODE PRE_TEST
    )
endif()

if(DK_BUILD_MATH)
    add_executable(dk_math_tests MathTests.cpp TransformTests.cpp)
    target_link_libraries(dk_math_tests PRIVATE dk::math Catch2::Catch2WithMain)
    dk_target_defaults(dk_math_tests)
    catch_discover_tests(dk_math_tests
        TEST_PREFIX "dk.math."
        WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
        DISCOVERY_MODE PRE_TEST
    )
endif()

catch_discover_tests(dk_core_tests
    TEST_PREFIX "dk.core."
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    DISCOVERY_MODE PRE_TEST
)

if(DK_BUILD_LOGGING)
    add_executable(dk_log_probe LogProbe.cpp)
    target_link_libraries(dk_log_probe PRIVATE dk::logging)
    dk_target_defaults(dk_log_probe)
    add_test(NAME dk.core.log_stream_separation
        COMMAND "${CMAKE_COMMAND}"
            "-DPROBE=$<TARGET_FILE:dk_log_probe>"
            "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/test-artifacts/$<CONFIG>/probe"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/LogOutputTest.cmake"
    )
endif()

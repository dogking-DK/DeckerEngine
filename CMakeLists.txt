cmake_minimum_required(VERSION 3.28)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
    message(FATAL_ERROR "In-source builds are not supported. Use a CMake preset or -B out/build/local.")
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
# Options that affect the toolchain must be evaluated before project().
include(Options)
include(Vcpkg)

project(DeckerEngine VERSION 0.1.0 DESCRIPTION "DeckerEngine" LANGUAGES CXX)

set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib/$<CONFIG>")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib/$<CONFIG>")

include(CompilerWarnings)
add_subdirectory(engine)
add_subdirectory(apps)
add_subdirectory(tools)
if(DK_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if(DK_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

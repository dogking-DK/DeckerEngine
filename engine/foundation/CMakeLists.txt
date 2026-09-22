set(CMAKE_FOLDER "${CMAKE_FOLDER}/Foundation")

add_subdirectory(core)
if(DK_BUILD_MATH)
    add_subdirectory(math)
endif()
if(DK_BUILD_IO)
    add_subdirectory(io)
endif()
# jobs and metadata will become targets as their modules are implemented.

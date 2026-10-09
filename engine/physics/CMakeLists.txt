set(CMAKE_FOLDER "${CMAKE_FOLDER}/Physics")
add_subdirectory(api)
add_subdirectory(xpbd)

if(DK_BUILD_PHYSICS_GPU)
    add_subdirectory(xpbd-gpu)
endif()

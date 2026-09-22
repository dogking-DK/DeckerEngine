add_subdirectory(foundation)
if(DK_BUILD_SCENE)
    add_subdirectory(assets)
    add_subdirectory(scene)
endif()
# Other module directories are reserved. Add their CMakeLists after module design.
if(DK_BUILD_FRAMEWORK)
    add_subdirectory(framework)
endif()

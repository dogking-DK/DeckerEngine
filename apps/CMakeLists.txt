set(CMAKE_FOLDER "Apps")

if(DK_BUILD_RUNNER)
    add_subdirectory(runner)
endif()
if(DK_BUILD_EDITOR)
    add_subdirectory(editor)
endif()
# ctl is reserved for M8.3.

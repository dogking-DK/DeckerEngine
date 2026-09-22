add_subdirectory(core)
if(DK_BUILD_MATH)
    add_subdirectory(math)
endif()
# io, jobs and metadata will become targets as their modules are implemented.

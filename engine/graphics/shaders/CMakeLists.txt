if(NOT DK_BUILD_MEMORY OR NOT DK_BUILD_IO)
    message(FATAL_ERROR "DK_BUILD_GRAPHICS_SHADERS requires DK_BUILD_MEMORY and DK_BUILD_IO")
endif()
find_package(slang CONFIG REQUIRED GLOBAL)
find_package(nlohmann_json CONFIG REQUIRED)
add_library(dk_graphics_shaders STATIC src/ShaderCompiler.cpp)
add_library(dk::graphics_shaders ALIAS dk_graphics_shaders)
target_sources(dk_graphics_shaders PUBLIC FILE_SET HEADERS BASE_DIRS include FILES
    include/dk/graphics/ShaderCompiler.hpp)
target_link_libraries(dk_graphics_shaders PUBLIC dk::graphics_shader_types
    PRIVATE dk::io dk::profiling slang::slang nlohmann_json::nlohmann_json)
dk_target_defaults(dk_graphics_shaders)

# vcpkg app-local copies DLL imports, but Slang also loads these data files.
function(dk_deploy_slang target)
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:slang::slang>" "$<TARGET_FILE_DIR:${target}>"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:slang::slang-glslang>" "$<TARGET_FILE_DIR:${target}>"
        COMMAND "${CMAKE_COMMAND}"
            "-DSOURCE_DIR=$<TARGET_FILE_DIR:slang::slang>"
            "-DDESTINATION_DIR=$<TARGET_FILE_DIR:${target}>"
            -P "${PROJECT_SOURCE_DIR}/cmake/DeploySlang.cmake"
        VERBATIM)
endfunction()

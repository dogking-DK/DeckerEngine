add_library(dk_graphics_shader_types STATIC src/ShaderArtifact.cpp)
add_library(dk::graphics_shader_types ALIAS dk_graphics_shader_types)
target_sources(dk_graphics_shader_types PUBLIC FILE_SET HEADERS BASE_DIRS include FILES
    include/dk/graphics/ShaderArtifact.hpp)
target_link_libraries(dk_graphics_shader_types PUBLIC dk::core dk::memory)
dk_target_defaults(dk_graphics_shader_types)

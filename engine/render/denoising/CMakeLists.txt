# Shared signal data only: no Graphics, shader compiler or SDK dependency.
add_library(dk_render_denoising_types INTERFACE)
add_library(dk::render_denoising_types ALIAS dk_render_denoising_types)
target_sources(dk_render_denoising_types INTERFACE FILE_SET HEADERS BASE_DIRS include
    FILES include/dk/render/DenoisingSignals.hpp)
target_link_libraries(dk_render_denoising_types INTERFACE dk::math)

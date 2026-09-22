find_package(flecs CONFIG REQUIRED)
if(NOT TARGET dk::math)
    message(FATAL_ERROR "DK_BUILD_SCENE requires DK_BUILD_MATH=ON")
endif()
add_library(dk_scene STATIC)
add_library(dk::scene ALIAS dk_scene)
target_sources(dk_scene
    PRIVATE src/SceneDocument.cpp src/Components.cpp
    PUBLIC FILE_SET HEADERS BASE_DIRS include FILES
        include/dk/scene/SceneDocument.hpp include/dk/scene/Components.hpp)
target_link_libraries(dk_scene PUBLIC dk::core dk::math
    PRIVATE $<IF:$<TARGET_EXISTS:flecs::flecs>,flecs::flecs,flecs::flecs_static>)
dk_target_defaults(dk_scene)

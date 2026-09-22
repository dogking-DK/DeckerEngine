find_package(flecs CONFIG REQUIRED)
add_library(dk_scene STATIC)
add_library(dk::scene ALIAS dk_scene)
target_sources(dk_scene
    PRIVATE src/SceneDocument.cpp
    PUBLIC FILE_SET HEADERS BASE_DIRS include FILES include/dk/scene/SceneDocument.hpp)
target_link_libraries(dk_scene PUBLIC dk::core
    PRIVATE $<IF:$<TARGET_EXISTS:flecs::flecs>,flecs::flecs,flecs::flecs_static>)
dk_target_defaults(dk_scene)

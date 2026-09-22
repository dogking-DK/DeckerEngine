find_package(flecs CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
if(NOT TARGET dk::io)
    message(FATAL_ERROR "DK_BUILD_SCENE requires DK_BUILD_IO=ON")
endif()
if(NOT TARGET dk::math)
    message(FATAL_ERROR "DK_BUILD_SCENE requires DK_BUILD_MATH=ON")
endif()
add_library(dk_scene STATIC)
add_library(dk::scene ALIAS dk_scene)
target_sources(dk_scene
    PRIVATE src/SceneDocument.cpp src/Components.cpp src/Project.cpp
    PUBLIC FILE_SET HEADERS BASE_DIRS include FILES
        include/dk/scene/SceneDocument.hpp include/dk/scene/Components.hpp include/dk/scene/Project.hpp)
target_link_libraries(dk_scene PUBLIC dk::core dk::math dk::io dk::asset_types
    PRIVATE nlohmann_json::nlohmann_json $<IF:$<TARGET_EXISTS:flecs::flecs>,flecs::flecs,flecs::flecs_static>)
dk_target_defaults(dk_scene)

find_package(nlohmann_json CONFIG REQUIRED)
add_library(dk_commands STATIC src/CommandRegistry.cpp src/Schema.cpp)
add_library(dk::commands ALIAS dk_commands)
target_sources(dk_commands PUBLIC FILE_SET HEADERS BASE_DIRS include FILES
    include/dk/commands/CommandRegistry.hpp include/dk/commands/Schema.hpp)
target_link_libraries(dk_commands PUBLIC dk::core nlohmann_json::nlohmann_json)
dk_target_defaults(dk_commands)

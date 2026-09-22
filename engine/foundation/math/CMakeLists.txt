find_package(Eigen3 5 CONFIG REQUIRED)

add_library(dk_math STATIC)
add_library(dk::math ALIAS dk_math)
target_sources(dk_math
    PRIVATE src/Math.cpp src/Transform.cpp
    PUBLIC FILE_SET HEADERS BASE_DIRS include FILES
        include/dk/math/Types.hpp
        include/dk/math/Math.hpp
        include/dk/math/Transform.hpp
)
target_link_libraries(dk_math PUBLIC dk::core Eigen3::Eigen)
dk_target_defaults(dk_math)

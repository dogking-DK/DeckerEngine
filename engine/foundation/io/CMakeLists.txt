add_library(dk_io STATIC)
add_library(dk::io ALIAS dk_io)
target_sources(dk_io
    PRIVATE src/Path.cpp src/File.cpp src/IoInternal.hpp
        src/AtomicFile.cpp src/AtomicFileInternal.hpp
    PUBLIC FILE_SET HEADERS BASE_DIRS include FILES
        include/dk/io/Path.hpp
        include/dk/io/File.hpp
)
target_link_libraries(dk_io PUBLIC dk::core)
dk_target_defaults(dk_io)

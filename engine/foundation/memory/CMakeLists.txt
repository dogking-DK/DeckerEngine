find_package(mimalloc 3 CONFIG REQUIRED)
find_package(Threads REQUIRED)
add_library(dk_memory STATIC src/MemorySystem.cpp src/Context.cpp src/Buffer.cpp src/Arena.cpp src/Pool.cpp)
add_library(dk::memory ALIAS dk_memory)
target_sources(dk_memory PUBLIC FILE_SET HEADERS BASE_DIRS include FILES
    include/dk/memory/Resource.hpp include/dk/memory/MemorySystem.hpp
    include/dk/memory/Context.hpp include/dk/memory/Allocator.hpp
    include/dk/memory/Buffer.hpp include/dk/memory/Containers.hpp include/dk/memory/SmartPtr.hpp
    include/dk/memory/Arena.hpp include/dk/memory/Pool.hpp
    include/dk/memory/PoolAllocator.hpp include/dk/memory/ObjectPool.hpp)
if(TARGET mimalloc)
    target_link_libraries(dk_memory PRIVATE mimalloc)
elseif(TARGET mimalloc-static)
    target_link_libraries(dk_memory PRIVATE mimalloc-static)
else()
    message(FATAL_ERROR "The mimalloc package must export mimalloc or mimalloc-static.")
endif()
target_link_libraries(dk_memory PRIVATE dk::profiling Threads::Threads)
dk_target_defaults(dk_memory)

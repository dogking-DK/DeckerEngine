#include <dk/profiling/Profiler.hpp>

namespace dk::profiling {

void set_thread_name(const char* name)
{
    tracy::SetThreadName(name);
}

bool is_connected() noexcept
{
    return TracyIsConnected;
}

} // namespace dk::profiling

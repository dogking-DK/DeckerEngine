#include <dk/profiling/Profiler.hpp>
#include <dk/profiling/Memory.hpp>

int main()
{
    static_assert(!dk::profiling::enabled());
    static_assert(!dk::profiling::is_connected());
    static_assert(!dk::profiling::memory_enabled());
    dk::profiling::record_allocation(nullptr, 0, dk::profiling::HeapCategory::general);
    dk::profiling::record_free(nullptr, dk::profiling::HeapCategory::general);
    dk::profiling::record_scratch_sample({}, {});
    dk::profiling::record_pool_sample(dk::profiling::PoolKind::local, {}, {});
    int effects = 0;
    DK_PROFILE_ZONE((++effects, "Disabled"));
    DK_PROFILE_ZONE_VALUE(++effects);
    DK_PROFILE_ZONE_TEXT((++effects, "unused"));
    DK_PROFILE_FRAME((++effects, "Frame"));
    DK_PROFILE_THREAD_NAME((++effects, "Thread"));
    // Disabled macros must not even require instrumentation-only names to exist.
    DK_PROFILE_ZONE_TEXT(unavailable_when_profiling_is_disabled());
    dk::profiling::set_thread_name("disabled");
    return effects == 0 ? 0 : 1;
}

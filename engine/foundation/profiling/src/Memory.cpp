#include <dk/profiling/Memory.hpp>

#if DK_ENABLE_PROFILING && DK_PROFILE_MEMORY
namespace dk::profiling {
namespace {
// One stable address per pool. Runtime instances aggregate within a category.
const char* pool_name(HeapCategory category) noexcept
{
    static constexpr char general[] = "dk/heap/general";
    static constexpr char assets[] = "dk/heap/assets";
    static constexpr char scene[] = "dk/heap/scene";
    static constexpr char render[] = "dk/heap/render";
    static constexpr char jobs[] = "dk/heap/jobs";
    static constexpr char other[] = "dk/heap/other";
    switch (category) {
    case HeapCategory::general: return general;
    case HeapCategory::assets: return assets;
    case HeapCategory::scene: return scene;
    case HeapCategory::render: return render;
    case HeapCategory::jobs: return jobs;
    default: return other;
    }
}
} // namespace
void record_allocation(const void* pointer, std::size_t size, HeapCategory category) noexcept
{
    TracyAllocNS(pointer, size, DK_PROFILE_CALLSTACK_DEPTH, pool_name(category));
}
void record_free(const void* pointer, HeapCategory category) noexcept
{
    TracyFreeNS(pointer, DK_PROFILE_CALLSTACK_DEPTH, pool_name(category));
}
} // namespace dk::profiling
#endif

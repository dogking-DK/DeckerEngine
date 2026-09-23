#include <dk/profiling/Memory.hpp>

#if DK_ENABLE_PROFILING && DK_PROFILE_MEMORY
#include <algorithm>
#include <mutex>
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
void record_scratch_sample(ScratchUsage previous, ScratchUsage current) noexcept
{
    // Serialize only safe-point samples, never arena bump allocations.
    static std::mutex mutex;
    static ScratchUsage total;
    static std::size_t peak = 0;
    static bool configured = false;
    static constexpr char used[] = "dk/scratch/used";
    static constexpr char retained[] = "dk/scratch/retained";
    static constexpr char backing[] = "dk/scratch/backing";
    static constexpr char sampled_peak[] = "dk/scratch/sampled-peak";
    std::lock_guard lock{mutex};
    total.used = total.used - previous.used + current.used;
    total.retained = total.retained - previous.retained + current.retained;
    total.backing = total.backing - previous.backing + current.backing;
    peak = (std::max)(peak, total.used);
    if (!configured) {
        for (const auto* name : {used, retained, backing, sampled_peak}) {
            TracyPlotConfig(name, tracy::PlotFormatType::Memory, true, true, 0);
        }
        configured = true;
    }
    TracyPlot(used, static_cast<std::int64_t>(total.used));
    TracyPlot(retained, static_cast<std::int64_t>(total.retained));
    TracyPlot(backing, static_cast<std::int64_t>(total.backing));
    TracyPlot(sampled_peak, static_cast<std::int64_t>(peak));
}
} // namespace dk::profiling
#endif

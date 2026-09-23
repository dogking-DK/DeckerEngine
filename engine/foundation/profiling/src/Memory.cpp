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
void record_pool_sample(PoolKind kind, PoolUsage previous, PoolUsage current) noexcept
{
    struct Totals { PoolUsage usage; std::size_t peak = 0; bool configured = false; };
    static std::mutex mutex;
    static Totals totals[2];
    static constexpr const char* names[2][4] = {
        {"dk/pool/local/live", "dk/pool/local/backing", "dk/pool/local/idle-backing", "dk/pool/local/sampled-peak"},
        {"dk/pool/shared/live", "dk/pool/shared/backing", "dk/pool/shared/idle-backing", "dk/pool/shared/sampled-peak"}};
    const auto index = kind == PoolKind::local ? 0 : 1;
    std::lock_guard lock{mutex};
    auto& total = totals[index];
    total.usage.live = total.usage.live - previous.live + current.live;
    total.usage.backing = total.usage.backing - previous.backing + current.backing;
    total.usage.idle_backing = total.usage.idle_backing - previous.idle_backing + current.idle_backing;
    total.peak = (std::max)(total.peak, total.usage.live);
    if (!total.configured) {
        for (const auto* name : names[index]) { TracyPlotConfig(name, tracy::PlotFormatType::Memory, true, true, 0); }
        total.configured = true;
    }
    TracyPlot(names[index][0], static_cast<std::int64_t>(total.usage.live));
    TracyPlot(names[index][1], static_cast<std::int64_t>(total.usage.backing));
    TracyPlot(names[index][2], static_cast<std::int64_t>(total.usage.idle_backing));
    TracyPlot(names[index][3], static_cast<std::int64_t>(total.peak));
}
} // namespace dk::profiling
#endif

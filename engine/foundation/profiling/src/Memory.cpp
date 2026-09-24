#include <dk/profiling/Memory.hpp>

#if DK_ENABLE_PROFILING && DK_PROFILE_MEMORY
#include <magic_enum/magic_enum.hpp>
#include <algorithm>
#include <array>
#include <mutex>
namespace dk::profiling {
namespace {
// Build labels at compile time: Tracy retains each pointer, so never use temporary strings.
template<class E, std::size_t P, std::size_t S>
consteval auto enum_labels(const char (&prefix)[P], const char (&suffix)[S])
{
    constexpr auto width = [] {
        std::size_t maximum = 0;
        for (const auto name : magic_enum::enum_names<E>()) { maximum = (std::max)(maximum, name.size()); }
        return maximum;
    }();
    std::array<std::array<char, P + width + S - 1>, magic_enum::enum_count<E>()> labels{};
    std::size_t index = 0;
    for (const auto name : magic_enum::enum_names<E>()) {
        auto& label = labels[index++];
        std::size_t offset = 0;
        for (std::size_t i = 0; i + 1 < P; ++i) { label[offset++] = prefix[i]; }
        for (const auto c : name) { label[offset++] = c; }
        for (std::size_t i = 0; i + 1 < S; ++i) { label[offset++] = suffix[i]; }
    }
    return labels;
}
// One stable address per pool. Runtime instances aggregate within a category.
const char* pool_name(HeapCategory category) noexcept
{
    static constexpr auto names = enum_labels<HeapCategory>("dk/heap/", "");
    const auto index = magic_enum::enum_index(category).value_or(*magic_enum::enum_index(HeapCategory::other));
    return names[index].data();
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
    static std::array<Totals, magic_enum::enum_count<PoolKind>()> totals{};
    static constexpr auto live = enum_labels<PoolKind>("dk/pool/", "/live");
    static constexpr auto backing = enum_labels<PoolKind>("dk/pool/", "/backing");
    static constexpr auto idle = enum_labels<PoolKind>("dk/pool/", "/idle-backing");
    static constexpr auto peak = enum_labels<PoolKind>("dk/pool/", "/sampled-peak");
    const auto index = magic_enum::enum_index(kind).value_or(*magic_enum::enum_index(PoolKind::shared));
    const std::array names{live[index].data(), backing[index].data(), idle[index].data(), peak[index].data()};
    std::lock_guard lock{mutex};
    auto& total = totals[index];
    total.usage.live = total.usage.live - previous.live + current.live;
    total.usage.backing = total.usage.backing - previous.backing + current.backing;
    total.usage.idle_backing = total.usage.idle_backing - previous.idle_backing + current.idle_backing;
    total.peak = (std::max)(total.peak, total.usage.live);
    if (!total.configured) {
        for (const auto* name : names) { TracyPlotConfig(name, tracy::PlotFormatType::Memory, true, true, 0); }
        total.configured = true;
    }
    TracyPlot(names[0], static_cast<std::int64_t>(total.usage.live));
    TracyPlot(names[1], static_cast<std::int64_t>(total.usage.backing));
    TracyPlot(names[2], static_cast<std::int64_t>(total.usage.idle_backing));
    TracyPlot(names[3], static_cast<std::int64_t>(total.peak));
}
} // namespace dk::profiling
#endif

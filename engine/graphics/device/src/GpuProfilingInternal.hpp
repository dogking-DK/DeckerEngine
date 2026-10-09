#pragma once
#include <dk/graphics/Device.hpp>
#include <dk/graphics/GpuProfiling.hpp>
#include <chrono>

namespace dk::graphics::detail {
struct QueueState;
struct BatchState;
struct Slot;
inline std::int64_t profile_clock() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
struct GpuEvent {
    std::uint32_t zone = 0;
    bool begin = false;
    std::int64_t tracy_time = 0;
    std::uint32_t thread = 0;
};
struct GpuProfileState {
    explicit GpuProfileState(memory::ResourceHandle heap, std::uint32_t capacity)
        : names(memory::Allocator<String>{heap}), timings(memory::Allocator<GpuTiming>{heap}),
          events(memory::Allocator<GpuEvent>{heap}), values(memory::Allocator<std::uint64_t>{heap}) {
        names.reserve(capacity); timings.reserve(capacity); events.reserve(2ull * capacity);
        values.resize(4ull * capacity); // value + availability for each endpoint.
    }
    Vector<String> names;
    Vector<GpuTiming> timings;
    Vector<GpuEvent> events;
    Vector<std::uint64_t> values;
    GpuProfileStatus status = GpuProfileStatus::pending;
    std::uint64_t submission = 0, connection = 0, alignment_ns = 0;
    std::uint32_t dropped = 0, valid_bits = 0;
    float period = 0;
    bool connected = false;
    bool pass_open = false, pass_dropped = false;
};
struct GpuProfileAccess {
    static GpuProfile make(std::shared_ptr<GpuProfileState> state) { return GpuProfile{std::move(state)}; }
};
struct GpuQuerySlot {
    vk::raii::QueryPool pool{nullptr};
};
struct GpuProfiler {
    explicit GpuProfiler(memory::ResourceHandle heap) : slots(memory::Allocator<GpuQuerySlot>{heap}) {}
    Vector<GpuQuerySlot> slots;
    GpuProfilingOptions options;
    std::uint32_t valid_bits = 0;
    float period = 0;
};
struct TracyGpuContext {
    int id = -1;
    std::uint64_t origin = 0, alignment_ns = 0;
};
void begin_gpu_profile(BatchState&);
void finish_gpu_profile(BatchState&) noexcept;
void collect_gpu_profile(QueueState&, Slot&) noexcept;
// Pure bounded wrap conversion, shared by collector and CPU unit tests.
inline std::uint64_t timestamp_delta(std::uint64_t begin, std::uint64_t end, std::uint32_t bits) noexcept {
    return (end - begin) & (bits == 64 ? UINT64_MAX : (std::uint64_t{1} << bits) - 1);
}
} // namespace dk::graphics::detail

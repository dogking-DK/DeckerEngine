#include "ResourceInternal.hpp"
#include <dk/profiling/Profiler.hpp>
#include <cmath>
#include <cstring>

namespace dk::graphics {
GpuProfileStatus GpuProfile::status() const noexcept { return state_ ? state_->status : GpuProfileStatus::disabled; }
std::span<const GpuTiming> GpuProfile::timings() const noexcept {
    return status() == GpuProfileStatus::ready ? std::span<const GpuTiming>{state_->timings} : std::span<const GpuTiming>{};
}
std::uint64_t GpuProfile::submission_value() const noexcept { return state_ ? state_->submission : 0; }
std::uint32_t GpuProfile::dropped_zones() const noexcept { return state_ ? state_->dropped : 0; }
double GpuProfile::timestamp_period_ns() const noexcept { return state_ ? state_->period : 0; }
std::uint32_t GpuProfile::timestamp_valid_bits() const noexcept { return state_ ? state_->valid_bits : 0; }
std::uint64_t GpuProfile::alignment_window_ns() const noexcept { return state_ ? state_->alignment_ns : 0; }

namespace detail {
namespace {
void event(BatchState& batch, std::uint32_t zone, bool begin) noexcept {
    auto& profile = *batch.profile;
    const auto query = static_cast<std::uint32_t>(profile.events.size());
    GpuEvent data{zone, begin};
#if DK_ENABLE_PROFILING
    data.tracy_time = tracy::Profiler::GetTime();
    data.thread = tracy::GetThreadHandle();
#endif
    profile.events.push_back(data); // Capacity was reserved before recording.
    batch.command().writeTimestamp2(vk::PipelineStageFlagBits2::eBottomOfPipe,
        *batch.queue->profiler->slots[batch.slot].pool, query);
    auto& timing = profile.timings[zone];
    (begin ? timing.cpu_begin_ns : timing.cpu_end_ns) = profile_clock();
}
void begin_zone(BatchState& batch, std::string_view name, GpuZoneKind kind) {
    auto& profile = *batch.profile;
    profile.names.emplace_back(name, memory::Allocator<char>{batch.queue->resource});
    profile.timings.push_back({profile.names.back(), kind});
    event(batch, static_cast<std::uint32_t>(profile.timings.size() - 1), true);
}
#if DK_ENABLE_PROFILING
// Tracy's fixed-version event protocol, isolated here. Native queries belong to our
// submission slots, so discarded command buffers never leave holes in Tracy's ring.
void publish_tracy(const QueueState& queue, const GpuProfileState& profile) noexcept {
    using namespace tracy;
    if (queue.tracy.id < 0 || !profile.connected || !GetProfiler().IsConnected() ||
        profile.connection != GetProfiler().ConnectionId()) return;
    const auto context = static_cast<std::uint8_t>(queue.tracy.id);
    for (std::size_t i = 0; i < profile.events.size(); ++i) {
        const auto& ev = profile.events[i];
        std::uint64_t source = 0;
        if (ev.begin) {
            const auto name = profile.timings[ev.zone].name;
            constexpr char file[] = "dk/graphics/GpuProfiling.cpp";
            constexpr char function[] = "Graph GPU pass";
            source = Profiler::AllocSourceLocation(1, file, sizeof(file)-1, function, sizeof(function)-1, name.data(), name.size());
        }
        auto* item = Profiler::QueueSerial();
        if (ev.begin) {
            MemWrite(&item->hdr.type, QueueType::GpuZoneBeginAllocSrcLocSerial);
            MemWrite(&item->gpuZoneBegin.cpuTime, ev.tracy_time);
            MemWrite(&item->gpuZoneBegin.srcloc, source);
            MemWrite(&item->gpuZoneBegin.thread, ev.thread);
            MemWrite(&item->gpuZoneBegin.queryId, static_cast<std::uint16_t>(i));
            MemWrite(&item->gpuZoneBegin.context, context);
        } else {
            MemWrite(&item->hdr.type, QueueType::GpuZoneEndSerial);
            MemWrite(&item->gpuZoneEnd.cpuTime, ev.tracy_time);
            MemWrite(&item->gpuZoneEnd.thread, ev.thread);
            MemWrite(&item->gpuZoneEnd.queryId, static_cast<std::uint16_t>(i));
            MemWrite(&item->gpuZoneEnd.context, context);
        }
        Profiler::QueueSerialFinish();
        item = Profiler::QueueSerial();
        MemWrite(&item->hdr.type, QueueType::GpuTime);
        MemWrite(&item->gpuTime.gpuTime, static_cast<std::int64_t>(timestamp_delta(queue.tracy.origin, profile.values[2*i], profile.valid_bits)));
        MemWrite(&item->gpuTime.queryId, static_cast<std::uint16_t>(i));
        MemWrite(&item->gpuTime.context, context);
        Profiler::QueueSerialFinish();
    }
}
Result<void> align_tracy(QueueState& queue, GpuProfiler& profiler) {
    if (queue.tracy.id != -1) return {};
    // Same non-calibrated fallback as Tracy Vulkan: one timestamp and an idle
    // confirmation. This setup is outside all measured submissions.
    auto& slot = queue.slots.front();
    const auto& device = queue.owner->device;
    slot.pool.reset({});
    slot.command.begin(vk::CommandBufferBeginInfo{vk::CommandBufferUsageFlagBits::eOneTimeSubmit});
    slot.command.resetQueryPool(*profiler.slots.front().pool, 0, 1);
    slot.command.writeTimestamp2(vk::PipelineStageFlagBits2::eBottomOfPipe, *profiler.slots.front().pool, 0);
    slot.command.end();
    const vk::CommandBufferSubmitInfo command{*slot.command};
    vk::SubmitInfo2 submit{}; submit.setCommandBufferInfos(command);
    const auto* api = device.logical_device().getDispatcher();
    const auto before = profile_clock();
    auto status = api->vkQueueSubmit2(static_cast<VkQueue>(*device.queue()), 1, reinterpret_cast<const VkSubmitInfo2*>(&submit), VK_NULL_HANDLE);
    if (status != VK_SUCCESS) return std::unexpected(queue.failure("GPU clock alignment submit", status));
    status = api->vkQueueWaitIdle(static_cast<VkQueue>(*device.queue()));
    if (status != VK_SUCCESS && status != VK_ERROR_DEVICE_LOST) status = api->vkDeviceWaitIdle(device.native_device());
    if (status != VK_SUCCESS && status != VK_ERROR_DEVICE_LOST) std::terminate(); // Cannot safely destroy in-flight candidate queries.
    if (status != VK_SUCCESS) return std::unexpected(queue.failure("GPU clock alignment wait", status));
    const auto cpu_time = tracy::Profiler::GetTime();
    const auto window = static_cast<std::uint64_t>(profile_clock() - before);
    std::uint64_t gpu_time = 0;
    status = api->vkGetQueryPoolResults(device.native_device(), static_cast<VkQueryPool>(*profiler.slots.front().pool),
        0, 1, sizeof(gpu_time), &gpu_time, sizeof(gpu_time), VK_QUERY_RESULT_64_BIT);
    if (status != VK_SUCCESS) return std::unexpected(queue.failure("GPU clock alignment query", status));
    const auto id = tracy::NextGpuContextId();
    if (id == 255 || id < 0) return std::unexpected(Error{ErrorCode::not_supported, "Tracy GPU context IDs exhausted"});
    queue.tracy = {id, gpu_time, window};
    using namespace tracy;
    auto* item = Profiler::QueueSerial();
    MemWrite(&item->hdr.type, QueueType::GpuNewContext);
    MemWrite(&item->gpuNewContext.cpuTime, cpu_time);
    MemWrite(&item->gpuNewContext.gpuTime, std::int64_t{0});
    std::memset(&item->gpuNewContext.thread, 0, sizeof(item->gpuNewContext.thread));
    MemWrite(&item->gpuNewContext.period, profiler.period);
    MemWrite(&item->gpuNewContext.context, static_cast<std::uint8_t>(id));
    MemWrite(&item->gpuNewContext.flags, GpuContextFlags{});
    MemWrite(&item->gpuNewContext.type, GpuContextType::Vulkan);
#ifdef TRACY_ON_DEMAND
    GetProfiler().DeferItem(*item);
#endif
    Profiler::QueueSerialFinish();
    return {};
}
#endif
} // namespace

void begin_gpu_profile(BatchState& batch) {
    auto& queue = *batch.queue;
    if (!queue.profiler) return;
    const auto& profiler = *queue.profiler;
    auto profile = memory::make_shared_in<GpuProfileState>(queue.resource, queue.resource, profiler.options.max_zones);
    profile->period = profiler.period; profile->valid_bits = profiler.valid_bits;
    profile->alignment_ns = queue.tracy.alignment_ns;
#if DK_ENABLE_PROFILING
    profile->connected = tracy::GetProfiler().IsConnected();
    profile->connection = tracy::GetProfiler().ConnectionId();
#endif
    batch.profile = std::move(profile);
    batch.command().resetQueryPool(*profiler.slots[batch.slot].pool, 0, 2 * profiler.options.max_zones);
    begin_zone(batch, "GPU submission", GpuZoneKind::submission);
}
void finish_gpu_profile(BatchState& batch) noexcept { if (batch.profile) event(batch, 0, false); }
void collect_gpu_profile(QueueState& queue, Slot& slot) noexcept {
    if (!slot.profile) return;
    auto& profile = *slot.profile;
    const auto index = static_cast<std::size_t>(&slot - queue.slots.data());
    const auto count = static_cast<std::uint32_t>(profile.events.size());
    const auto status = queue.api.query(queue.owner->device.native_device(), static_cast<VkQueryPool>(*queue.profiler->slots[index].pool),
        0, count, static_cast<std::size_t>(count) * 2 * sizeof(std::uint64_t), profile.values.data(),
        2 * sizeof(std::uint64_t), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
    bool available = status == VK_SUCCESS;
    for (std::uint32_t i = 0; i < count; ++i) available = available && profile.values[2ull*i+1] != 0;
    profile.status = status == VK_ERROR_DEVICE_LOST ? GpuProfileStatus::device_lost : GpuProfileStatus::unavailable;
    if (status == VK_ERROR_DEVICE_LOST) queue.owner->lost = true;
    if (available) {
        // Store endpoint indices without any allocation at completion.
        std::uint64_t submission_begin = 0, pass_begin = 0;
        for (std::uint32_t i = 0; i < count; ++i) {
            const auto& ev = profile.events[i];
            auto& begin = ev.zone == 0 ? submission_begin : pass_begin;
            if (ev.begin) begin = profile.values[2ull*i];
            else profile.timings[ev.zone].elapsed_ns = static_cast<double>(timestamp_delta(begin, profile.values[2ull*i], profile.valid_bits)) * profile.period;
        }
        profile.status = GpuProfileStatus::ready;
#if DK_ENABLE_PROFILING
        publish_tracy(queue, profile);
#endif
    }
    slot.profile.reset();
}
} // namespace detail

Result<void> SubmissionQueue::configure_gpu_profiling(GpuProfilingOptions options) {
    DK_PROFILE_ZONE("graphics.profiling.configure");
    if (auto status = state_->accepting(); !status) return status;
    if (!options.max_zones || options.max_zones > 32768)
        return std::unexpected(Error{ErrorCode::invalid_argument, "GPU profile capacity must be 1..32768 zones"});
    for (const auto& slot : state_->slots) if (slot.phase != detail::SlotPhase::free)
        return std::unexpected(Error{ErrorCode::conflict, "GPU profiling configuration requires all slots retired"});
    if (!options.enabled) { state_->profiler.reset(); return {}; }
    const auto& adapter = device().adapter();
    const auto bits = adapter.queues[device().queue_family()].timestampValidBits;
    const auto period = adapter.properties.limits.timestampPeriod;
    if (!bits || bits > 64 || !std::isfinite(period) || period <= 0)
        return std::unexpected(Error{ErrorCode::not_supported, "queue has no valid timestamp clock"});
    if (state_->profiler && state_->profiler->options.max_zones == options.max_zones) return {};
    auto candidate = memory::make_shared_in<detail::GpuProfiler>(state_->resource, state_->resource);
    candidate->options = options; candidate->valid_bits = bits; candidate->period = period;
    try {
        candidate->slots.resize(state_->slots.size());
        for (auto& slot : candidate->slots)
            slot.pool = vk::raii::QueryPool{device().logical_device(), vk::QueryPoolCreateInfo{{}, vk::QueryType::eTimestamp, 2 * options.max_zones}};
#if DK_ENABLE_PROFILING
        if (auto aligned = detail::align_tracy(*state_, *candidate); !aligned) return aligned;
#endif
    } catch (const vk::SystemError& error) {
        return std::unexpected(state_->failure("create GPU profiling resources", static_cast<VkResult>(error.code().value())));
    }
    state_->profiler = std::move(candidate);
    return {};
}
Result<void> CommandBatch::begin_gpu_zone(std::string_view name, GpuZoneKind kind) {
    if (!state_) return std::unexpected(Error{ErrorCode::invalid_state, "empty command batch"});
    if (auto valid = state_->valid(); !valid) return valid;
    if (!state_->profile) return {};
    auto& profile = *state_->profile;
    if (profile.pass_open || state_->rendering)
        return std::unexpected(Error{ErrorCode::invalid_state, "GPU pass zones cannot nest or begin inside rendering"});
    if (name.empty() || name.size() > 1024 || kind == GpuZoneKind::submission)
        return std::unexpected(Error{ErrorCode::invalid_argument, "GPU pass name must be 1..1024 bytes and kind cannot be submission"});
    const bool full = profile.timings.size() == state_->queue->profiler->options.max_zones;
    if (full) ++profile.dropped;
    else detail::begin_zone(*state_, name, kind);
    profile.pass_open = true; profile.pass_dropped = full;
    return {};
}
Result<void> CommandBatch::end_gpu_zone() {
    if (!state_) return std::unexpected(Error{ErrorCode::invalid_state, "empty command batch"});
    if (auto valid = state_->valid(); !valid) return valid;
    if (!state_->profile) return {};
    auto& profile = *state_->profile;
    if (!profile.pass_open || state_->rendering)
        return std::unexpected(Error{ErrorCode::invalid_state, "GPU pass zone is not open or rendering has not ended"});
    if (!profile.pass_dropped) detail::event(*state_, static_cast<std::uint32_t>(profile.timings.size() - 1), false);
    profile.pass_open = false; profile.pass_dropped = false;
    return {};
}
} // namespace dk::graphics

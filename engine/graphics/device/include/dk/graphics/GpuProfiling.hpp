#pragma once
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

namespace dk::graphics {
enum class GpuProfileStatus { disabled, pending, ready, unavailable, device_lost };
enum class GpuZoneKind { submission, compute, draw, transfer, other };
struct GpuProfilingOptions {
    bool enabled = false;
    std::uint32_t max_zones = 8192; // Includes submission; bounded to 1..32768.
};
struct GpuTiming {
    std::string_view name; // Owned by the profile. Valid while a profile owner survives.
    GpuZoneKind kind = GpuZoneKind::other;
    double elapsed_ns = 0;
    std::int64_t cpu_begin_ns = 0, cpu_end_ns = 0; // steady_clock epoch, CPU recording only.
};
namespace detail { struct GpuProfileState; struct GpuProfileAccess; }
class GpuProfile final {
public:
    GpuProfile() = default;
    [[nodiscard]] GpuProfileStatus status() const noexcept;
    [[nodiscard]] std::span<const GpuTiming> timings() const noexcept; // Only ready exposes values.
    [[nodiscard]] std::uint64_t submission_value() const noexcept;
    [[nodiscard]] std::uint32_t dropped_zones() const noexcept;
    [[nodiscard]] double timestamp_period_ns() const noexcept;
    [[nodiscard]] std::uint32_t timestamp_valid_bits() const noexcept;
    [[nodiscard]] std::uint64_t alignment_window_ns() const noexcept;
private:
    friend struct detail::GpuProfileAccess;
    explicit GpuProfile(std::shared_ptr<detail::GpuProfileState> state) : state_(std::move(state)) {}
    std::shared_ptr<detail::GpuProfileState> state_; // CPU data only; does not retain the queue/device.
};
} // namespace dk::graphics

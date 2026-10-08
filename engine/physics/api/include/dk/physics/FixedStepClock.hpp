#pragma once
#include <dk/core/Result.hpp>
#include <cstdint>

namespace dk {
struct FixedStepConfig {
    std::int64_t fixed_dt_ns = 16666667;
    std::uint32_t max_catch_up_steps = 8;
};
struct FixedStepState {
    std::uint64_t steps = 0;
    std::int64_t simulated_time_ns = 0;
    std::int64_t accumulator_ns = 0;
    std::int64_t dropped_time_ns = 0;
    bool operator==(const FixedStepState&) const = default;
};
// Owner-thread integer clock. No solver or wall-clock access.
class FixedStepClock final {
public:
    [[nodiscard]] static Result<FixedStepClock> create(FixedStepConfig config = {});
    [[nodiscard]] FixedStepConfig config() const noexcept { return config_; }
    [[nodiscard]] FixedStepState state() const noexcept { return state_; }
    // Returns the number of fixed ticks actually advanced, at most max_catch_up_steps.
    [[nodiscard]] Result<std::uint32_t> advance(std::int64_t elapsed_ns);
    [[nodiscard]] Result<void> step(std::uint32_t count = 1);
    void discard_fraction() noexcept { state_.accumulator_ns = 0; }
private:
    explicit FixedStepClock(FixedStepConfig config) : config_{config} {}
    FixedStepConfig config_;
    FixedStepState state_;
};
}

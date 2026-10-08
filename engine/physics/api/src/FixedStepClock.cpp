#include <dk/physics/FixedStepClock.hpp>
#include <algorithm>
#include <limits>

namespace dk {
namespace {
constexpr auto maximum = std::numeric_limits<std::int64_t>::max();
}
Result<FixedStepClock> FixedStepClock::create(FixedStepConfig config) {
    if (config.fixed_dt_ns < 1000000 || config.fixed_dt_ns > 1000000000
        || config.max_catch_up_steps < 1 || config.max_catch_up_steps > 64)
        return std::unexpected(Error{ErrorCode::invalid_argument, "Invalid fixed step configuration"});
    return FixedStepClock{config};
}
Result<std::uint32_t> FixedStepClock::advance(std::int64_t elapsed_ns) {
    if (elapsed_ns < 0 || elapsed_ns > maximum - state_.accumulator_ns)
        return std::unexpected(Error{ErrorCode::invalid_argument, "Elapsed simulation time is out of range"});
    const auto total = state_.accumulator_ns + elapsed_ns;
    const auto due = total / config_.fixed_dt_ns;
    const auto count = std::min(due, static_cast<std::int64_t>(config_.max_catch_up_steps));
    const auto simulated = count * config_.fixed_dt_ns;
    const auto dropped = (due - count) * config_.fixed_dt_ns;
    if (simulated > maximum - state_.simulated_time_ns || dropped > maximum - state_.dropped_time_ns)
        return std::unexpected(Error{ErrorCode::invalid_state, "Simulation clock exhausted"});
    // All validation precedes publication; steps is bounded by the int64 time / dt.
    state_.steps += static_cast<std::uint64_t>(count);
    state_.simulated_time_ns += simulated;
    state_.dropped_time_ns += dropped;
    state_.accumulator_ns = total % config_.fixed_dt_ns;
    return static_cast<std::uint32_t>(count);
}
Result<void> FixedStepClock::step(std::uint32_t count) {
    if (count < 1 || count > 10000)
        return std::unexpected(Error{ErrorCode::invalid_argument, "Simulation step count must be 1-10000"});
    const auto elapsed = static_cast<std::int64_t>(count) * config_.fixed_dt_ns;
    if (elapsed > maximum - state_.simulated_time_ns)
        return std::unexpected(Error{ErrorCode::invalid_state, "Simulation clock exhausted"});
    state_.steps += count;
    state_.simulated_time_ns += elapsed;
    return {};
}
}

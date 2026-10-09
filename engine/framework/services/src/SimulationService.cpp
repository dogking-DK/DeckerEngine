#include <dk/services/SimulationService.hpp>
#include <algorithm>

namespace dk {
Result<void> SimulationService::start(const SceneService& edit, EditGuard guard, FixedStepConfig config,
                                      bool paused, std::optional<TimePoint> now, std::optional<ClothConfig> cloth) {
    if (play_) return std::unexpected(Error{ErrorCode::invalid_state, "Stop the active simulation before starting"});
    auto clock = FixedStepClock::create(config);
    if (!clock) return std::unexpected(clock.error());
    std::optional<XpbdSolver> solver;
    if (cloth) {
        if (config.fixed_dt_ns > 33333333)
            return std::unexpected(Error{ErrorCode::invalid_argument, "XPBD fixed_dt_ns must be 1000000..33333333"});
        auto created = XpbdSolver::cloth(*cloth);
        if (!created) return std::unexpected(created.error());
        solver = std::move(*created);
    }
    auto input = edit.read_snapshot(guard);
    if (!input) return std::unexpected(input.error());
    auto scene = SceneDocument::stage(input->scene);
    if (!scene) return std::unexpected(scene.error());
    auto id = SimulationId::generate();
    if (!id) return std::unexpected(id.error());
    auto candidate = std::make_unique<PlayWorld>(PlayWorld{*id, input->state, std::move(input->project),
        std::move(input->manifest), std::move(*scene), *clock, cloth, std::move(solver), paused, {}, {}});
    candidate->last_pump = now.value_or(Clock::now());
    play_ = std::move(candidate);
    return {};
}
Result<void> SimulationService::check_run(SimulationId id) const {
    if (id.is_nil()) return std::unexpected(Error{ErrorCode::invalid_argument, "Simulation ID must not be nil"});
    if (!play_) return std::unexpected(Error{ErrorCode::invalid_state, "No active simulation"});
    if (id != play_->id) return std::unexpected(Error{ErrorCode::conflict, "Simulation ID does not match active run"});
    return {};
}
Result<void> SimulationService::pause(SimulationId id) {
    auto valid = check_run(id); if (!valid) return valid;
    if (!play_->paused) { play_->paused = true; play_->clock.discard_fraction(); }
    return {};
}
Result<void> SimulationService::resume(SimulationId id, TimePoint now) {
    auto valid = check_run(id); if (!valid) return valid;
    if (play_->fault) return std::unexpected(Error{ErrorCode::invalid_state, "Stop and restart the faulted simulation"});
    if (play_->paused) { play_->last_pump = now; play_->paused = false; }
    return {};
}
Result<void> SimulationService::step(SimulationId id, std::uint32_t count) {
    auto valid = check_run(id); if (!valid) return valid;
    if (!play_->paused || play_->fault)
        return std::unexpected(Error{ErrorCode::invalid_state, "Single stepping requires a paused healthy simulation"});
    auto clock = play_->clock;
    auto advanced = clock.step(count); if (!advanced) return advanced;
    if (play_->solver) {
        advanced = play_->solver->advance(clock.config().fixed_dt_ns, count);
        if (!advanced) return advanced;
    }
    play_->clock = clock;
    return {};
}
Result<void> SimulationService::stop(SimulationId id) {
    auto valid = check_run(id); if (!valid) return valid;
    play_.reset();
    return {};
}
SimulationRunState SimulationService::run_state() const {
    return {play_->id, play_->source, play_->clock.config(), play_->clock.state(), play_->fault, play_->cloth,
        play_->solver ? std::optional{play_->solver->metrics()} : std::nullopt};
}
SimulationState SimulationService::state() const {
    if (!play_) return {SimulationMode::edit, {}};
    return {play_->paused ? SimulationMode::paused : SimulationMode::running, run_state()};
}
Result<PlaySceneSnapshot> SimulationService::read_snapshot(SimulationId id) const {
    auto valid = check_run(id); if (!valid) return std::unexpected(valid.error());
    auto scene = play_->scene->snapshot();
    if (!scene) return std::unexpected(scene.error());
    return PlaySceneSnapshot{run_state(), play_->project, std::move(*scene), play_->manifest};
}
Result<PlayParticleSnapshot> SimulationService::read_particles(SimulationId id) const {
    auto valid = check_run(id); if (!valid) return std::unexpected(valid.error());
    if (!play_->solver) return std::unexpected(Error{ErrorCode::invalid_state, "The active simulation has no particle solver"});
    return PlayParticleSnapshot{run_state(), play_->solver->snapshot()};
}
Result<SimulationParticlePage> SimulationService::particle_page(SimulationId id, std::size_t offset, std::size_t limit) const {
    auto valid = check_run(id); if (!valid) return std::unexpected(valid.error());
    if (!play_->solver) return std::unexpected(Error{ErrorCode::invalid_state, "The active simulation has no particle solver"});
    const auto positions = play_->solver->positions(); const auto velocities = play_->solver->velocities();
    if (offset > positions.size() || limit < 1 || limit > 256)
        return std::unexpected(Error{ErrorCode::invalid_argument, "Invalid simulation particle page"});
    const auto count = std::min(limit, positions.size()-offset);
    const auto p = positions.subspan(offset, count); const auto v = velocities.subspan(offset, count);
    return SimulationParticlePage{run_state(), offset, positions.size(), {p.begin(), p.end()}, {v.begin(), v.end()}};
}
void SimulationService::pump(TimePoint now) {
    if (!play_ || play_->paused) return;
    const auto previous = play_->last_pump.time_since_epoch();
    const auto current = now.time_since_epoch();
    if (current < previous || (previous < Clock::duration::zero() && current > Clock::duration::max() + previous)) {
        play_->paused = true; play_->fault = ErrorCode::invalid_argument; return;
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(now - play_->last_pump).count();
    auto clock = play_->clock;
    const auto advanced = clock.advance(elapsed);
    if (!advanced) { play_->paused = true; play_->fault = advanced.error().code; return; }
    if (play_->solver && *advanced > 0) {
        const auto solved = play_->solver->advance(clock.config().fixed_dt_ns, *advanced);
        if (!solved) { play_->paused = true; play_->fault = solved.error().code; return; }
    }
    play_->clock = clock;
    play_->last_pump = now;
}
SimulationService::TimePoint SimulationService::next_deadline(TimePoint fallback) const {
    if (!play_ || play_->paused) return fallback;
    const auto remaining = std::chrono::duration_cast<Clock::duration>(std::chrono::nanoseconds{
        play_->clock.config().fixed_dt_ns - play_->clock.state().accumulator_ns});
    if (play_->last_pump > TimePoint::max() - remaining) return fallback;
    return std::min(fallback, play_->last_pump + remaining);
}
}

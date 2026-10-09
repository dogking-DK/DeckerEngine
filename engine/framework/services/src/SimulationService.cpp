#include <dk/services/SimulationService.hpp>
#include <algorithm>
#include "AsyncSimulation.hpp"
#ifdef DK_SIMULATION_GPU
#include "GpuSimulation.hpp"
#endif

namespace dk {
Result<void> SimulationService::set_gpu_device(std::shared_ptr<const graphics::Device> device) {
    if (play_) return std::unexpected(Error{ErrorCode::invalid_state,"Stop simulation before replacing its GPU device"});
#ifdef DK_SIMULATION_GPU
    if (device && device->queue_count()<2)
        return std::unexpected(Error{ErrorCode::not_supported,"Simulation requires a dedicated secondary queue"});
#else
    if (device) return std::unexpected(Error{ErrorCode::not_supported,"GPU simulation is not compiled"});
#endif
    gpu_device_=std::move(device);
    return {};
}
Result<void> SimulationService::run(const SceneService& edit, EditGuard guard, std::uint32_t count,
    ClothConfig cloth, bool gpu, std::int64_t dt, std::uint32_t batch, bool paused) {
    if (!count || count > 1000000 || !batch || batch > 8 || dt < 1000000 || dt > 33333333)
        return std::unexpected(Error{ErrorCode::invalid_argument,"Task needs count 1..1000000, batch 1..8 and XPBD dt"});
#ifndef DK_SIMULATION_GPU
    if (gpu) return std::unexpected(Error{ErrorCode::not_supported,"GPU simulation is not compiled"});
#endif
    // Reuse validated CPU/scene candidate construction. No worker exists until it succeeds;
    // remove the unpublished candidate again if worker allocation/creation throws.
    auto created = start(edit,guard,{dt,batch},true,{},cloth,false);
    if (!created) return created;
    auto candidate = std::move(play_);
    candidate->task_gpu = gpu;
    candidate->paused = false;
    candidate->task = detail::AsyncSimulation::create(*candidate->solver,gpu,dt,count,batch,gpu_device_,paused);
    candidate->progress = SimulationTaskState{paused ? SimulationTaskStatus::pausing : SimulationTaskStatus::initializing,count,0,0,batch};
    play_ = std::move(candidate);
    return {};
}
Result<void> SimulationService::cancel(SimulationId id) {
    auto valid = check_run(id); if (!valid) return valid;
    if (!play_->task) return std::unexpected(Error{ErrorCode::invalid_state,"Cancel requires a finite simulation task"});
    auto result = play_->task->cancel(); pump(); return result;
}
Result<void> SimulationService::start(const SceneService& edit, EditGuard guard, FixedStepConfig config,
                                      bool paused, std::optional<TimePoint> now, std::optional<ClothConfig> cloth, bool gpu) {
    if (play_) return std::unexpected(Error{ErrorCode::invalid_state, "Stop the active simulation before starting"});
    if (gpu && (!cloth || config.max_catch_up_steps > 8))
        return std::unexpected(Error{ErrorCode::invalid_argument, "GPU simulation requires cloth and max_catch_up_steps <= 8"});
#ifndef DK_SIMULATION_GPU
    if (gpu) return std::unexpected(Error{ErrorCode::not_supported, "GPU simulation is not compiled"});
#endif
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
        std::move(input->manifest), std::move(*scene), *clock, cloth, std::move(solver), paused, {}, {}, {}});
#ifdef DK_SIMULATION_GPU
    if (gpu) {
        auto created = detail::GpuSimulation::create(*candidate->solver,{},gpu_device_);
        if (!created) return std::unexpected(created.error());
        candidate->gpu = std::move(*created);
    }
#endif
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
    if (play_->task) { auto result = play_->task->pause(); pump(); return result; }
    if (!play_->paused) { play_->paused = true; play_->clock.discard_fraction(); }
    return {};
}
Result<void> SimulationService::resume(SimulationId id, TimePoint now) {
    auto valid = check_run(id); if (!valid) return valid;
    if (play_->fault) return std::unexpected(Error{ErrorCode::invalid_state, "Stop and restart the faulted simulation"});
    if (play_->task) { auto result = play_->task->resume(); pump(); return result; }
    if (play_->paused) { play_->last_pump = now; play_->paused = false; }
    return {};
}
Result<void> SimulationService::step(SimulationId id, std::uint32_t count) {
    auto valid = check_run(id); if (!valid) return valid;
    if (play_->task) { auto result = play_->task->step(count); pump(); return result; }
    if (!play_->paused || play_->fault)
        return std::unexpected(Error{ErrorCode::invalid_state, "Single stepping requires a paused healthy simulation"});
    auto clock = play_->clock;
    auto advanced = clock.step(count); if (!advanced) return advanced;
    advanced = advance_solver(clock, count);
    if (!advanced) return advanced;
    play_->clock = clock;
    return {};
}
Result<void> SimulationService::advance_solver(const FixedStepClock& clock, std::uint32_t count) {
    if (!count || !play_->solver) return {};
#ifdef DK_SIMULATION_GPU
    if (play_->gpu) {
        auto result = play_->gpu->step(clock.config().fixed_dt_ns, count);
        // A completed submit is irreversible even if its wait/device subsequently fails.
        if (!result && play_->gpu->steps() != play_->clock.state().steps) {
            play_->clock = clock; play_->paused = true; play_->fault = result.error().code;
        }
        return result;
    }
#endif
    return play_->solver->advance(clock.config().fixed_dt_ns, count);
}
void SimulationService::request_shutdown() noexcept {
    if (play_ && play_->task) play_->task->stop();
    else play_.reset();
}
Result<void> SimulationService::stop(SimulationId id) {
    auto valid = check_run(id); if (!valid) return valid;
    if (play_->task) { play_->task->stop(); pump(); return {}; }
    play_.reset();
    return {};
}
SimulationRunState SimulationService::run_state() const {
    return {play_->id, play_->source, play_->clock.config(), play_->clock.state(), play_->fault, play_->cloth,
        play_->solver && !play_->gpu && !play_->task ? std::optional{play_->solver->metrics()} : std::nullopt,
        bool(play_->gpu) || play_->task_gpu, play_->progress};
}
SimulationState SimulationService::state() const {
    if (!play_) return {SimulationMode::edit, {}};
    if (play_->progress) {
        SimulationMode mode = SimulationMode::paused;
        switch (play_->progress->status) {
        case SimulationTaskStatus::initializing: mode = SimulationMode::initializing; break;
        case SimulationTaskStatus::running: mode = SimulationMode::running; break;
        case SimulationTaskStatus::pausing: mode = SimulationMode::pausing; break;
        case SimulationTaskStatus::cancelling: mode = SimulationMode::cancelling; break;
        case SimulationTaskStatus::stopping: mode = SimulationMode::stopping; break;
        default: break;
        }
        return {mode,run_state()};
    }
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
    if (play_->task) {
        // Backend completion can race ahead of the last owner pump. Do not label a newer
        // particle array with the owner's older clock; require a published stable boundary.
        if (!play_->paused || play_->fault)
            return std::unexpected(Error{ErrorCode::invalid_state,"Task diagnostics require a published healthy stable boundary"});
        auto snapshot = play_->task->read();
        if (!snapshot) return std::unexpected(snapshot.error());
        auto run = run_state(); run.metrics = snapshot->metrics;
        return PlayParticleSnapshot{run,std::move(*snapshot)};
    }
    auto snapshot = play_->solver->snapshot();
#ifdef DK_SIMULATION_GPU
    if (play_->gpu) {
        if (!play_->paused || play_->fault)
            return std::unexpected(Error{ErrorCode::invalid_state, "GPU readback requires a paused healthy simulation"});
        auto particles = play_->gpu->read(play_->clock.config().fixed_dt_ns);
        if (!particles) {
            play_->fault = particles.error().code;
            return std::unexpected(particles.error());
        }
        auto metrics = play_->solver->evaluate(particles->positions, particles->velocities);
        if (!metrics) { play_->fault = metrics.error().code; return std::unexpected(metrics.error()); }
        snapshot.positions = std::move(particles->positions); snapshot.velocities = std::move(particles->velocities);
        snapshot.metrics = *metrics;
    }
#endif
    auto run = run_state(); run.metrics = snapshot.metrics;
    return PlayParticleSnapshot{run, std::move(snapshot)};
}
Result<SimulationParticlePage> SimulationService::particle_page(SimulationId id, std::size_t offset, std::size_t limit) const {
    auto valid = check_run(id); if (!valid) return std::unexpected(valid.error());
    if (!play_->solver) return std::unexpected(Error{ErrorCode::invalid_state, "The active simulation has no particle solver"});
    if (offset > play_->solver->positions().size() || limit < 1 || limit > 256)
        return std::unexpected(Error{ErrorCode::invalid_argument, "Invalid simulation particle page"});
    auto snapshot = read_particles(id); if (!snapshot) return std::unexpected(snapshot.error());
    const auto positions = std::span{snapshot->data.positions}; const auto velocities = std::span{snapshot->data.velocities};
    const auto count = std::min(limit, positions.size()-offset);
    const auto p = positions.subspan(offset, count); const auto v = velocities.subspan(offset, count);
    return SimulationParticlePage{snapshot->run, offset, positions.size(), {p.begin(), p.end()}, {v.begin(), v.end()}};
}
void SimulationService::pump(TimePoint now) {
    if (play_ && play_->task) {
        const auto state = play_->task->state();
        if (state.status == SimulationTaskStatus::stopping && play_->task->closed()) { play_.reset(); return; }
        // Worker completion is only published here on the service owner thread.
        auto clock = play_->clock;
        if (state.completed_steps < clock.state().steps) throw std::logic_error{"Task completion moved backwards"};
        while (clock.state().steps < state.completed_steps) {
            auto advanced = clock.step(static_cast<std::uint32_t>(std::min<std::uint64_t>(10000,state.completed_steps-clock.state().steps)));
            if (!advanced) throw std::logic_error{"Task completion exceeds the validated clock budget"};
        }
        play_->clock = clock; play_->progress = state; play_->fault = state.error;
        play_->paused = state.status == SimulationTaskStatus::paused || state.status == SimulationTaskStatus::succeeded ||
            state.status == SimulationTaskStatus::cancelled || state.status == SimulationTaskStatus::failed;
        return;
    }
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
        const auto solved = advance_solver(clock, *advanced);
        if (!solved) { play_->paused = true; play_->fault = solved.error().code; return; }
    }
    play_->clock = clock;
    play_->last_pump = now;
}
SimulationService::TimePoint SimulationService::next_deadline(TimePoint fallback) const {
    if (play_ && play_->task) return play_->paused ? fallback : std::min(fallback,Clock::now()+std::chrono::milliseconds{5});
    if (!play_ || play_->paused) return fallback;
    const auto remaining = std::chrono::duration_cast<Clock::duration>(std::chrono::nanoseconds{
        play_->clock.config().fixed_dt_ns - play_->clock.state().accumulator_ns});
    if (play_->last_pump > TimePoint::max() - remaining) return fallback;
    return std::min(fallback, play_->last_pump + remaining);
}
}

#include "AsyncSimulation.hpp"
#include <dk/profiling/Profiler.hpp>
#include <algorithm>
#include <condition_variable>
#include <future>
#include <mutex>
#include <thread>

namespace dk {
std::string_view simulation_task_status_name(SimulationTaskStatus status) noexcept {
    switch (status) {
    case SimulationTaskStatus::initializing: return "initializing";
    case SimulationTaskStatus::running: return "running";
    case SimulationTaskStatus::pausing: return "pausing";
    case SimulationTaskStatus::paused: return "paused";
    case SimulationTaskStatus::cancelling: return "cancelling";
    case SimulationTaskStatus::cancelled: return "cancelled";
    case SimulationTaskStatus::succeeded: return "succeeded";
    case SimulationTaskStatus::failed: return "failed";
    case SimulationTaskStatus::stopping: return "stopping";
    }
    return "failed";
}
namespace detail {
namespace {
class Backend final : public SimulationTaskBackend {
public:
    Backend(XpbdSolver input, bool gpu) : cpu_(std::move(input)), use_gpu_(gpu) {}
    Result<void> initialize() override {
#ifdef DK_SIMULATION_GPU
        if (use_gpu_) {
            auto result = GpuSimulation::create(cpu_);
            if (!result) return std::unexpected(result.error());
            gpu_ = std::move(*result);
        }
#endif
        return {};
    }
    Result<void> submit(std::int64_t dt, std::uint32_t count) override {
        dt_ = dt;
#ifdef DK_SIMULATION_GPU
        if (gpu_) return gpu_->submit(dt,count);
#endif
        auto result = cpu_.advance(dt,count);
        if (result) steps_ += count;
        return result;
    }
    Result<bool> poll() override {
#ifdef DK_SIMULATION_GPU
        if (gpu_) return gpu_->poll();
#endif
        return true;
    }
    std::uint64_t submitted() const override {
#ifdef DK_SIMULATION_GPU
        if (gpu_) return gpu_->steps();
#endif
        return steps_;
    }
    Result<XpbdSnapshot> read() override {
        auto result = cpu_.snapshot();
#ifdef DK_SIMULATION_GPU
        if (gpu_) {
            auto particles = gpu_->read(dt_);
            if (!particles) return std::unexpected(particles.error());
            auto metrics = cpu_.evaluate(particles->positions,particles->velocities);
            if (!metrics) return std::unexpected(metrics.error());
            result.positions = std::move(particles->positions); result.velocities = std::move(particles->velocities);
            result.metrics = *metrics;
        }
#endif
        return result;
    }
#ifdef DK_SIMULATION_GPU
    Result<SimulationImage> capture(std::int64_t dt, const ClothConfig& cloth, std::uint32_t width, std::uint32_t height) override {
        auto renderer = gpu_;
        if (!renderer) {
            auto created = GpuSimulation::create(cpu_);
            if (!created) return std::unexpected(created.error());
            renderer = std::move(*created);
        }
        return renderer->capture(dt,cloth,width,height);
    }
#endif
private:
    XpbdSolver cpu_;
    bool use_gpu_;
    std::int64_t dt_ = 10000000;
    std::uint64_t steps_ = 0;
#ifdef DK_SIMULATION_GPU
    std::shared_ptr<GpuSimulation> gpu_;
#endif
};
}
struct AsyncSimulation::Impl {
    enum class Request { run, pause, cancel, stop };
    mutable std::mutex mutex;
    std::condition_variable changed;
    SimulationTaskState progress;
    Request requested = Request::run;
    std::optional<SimulationTaskStatus> terminal;
    std::exception_ptr fatal;
    bool finished = false;
    std::function<void(SimulationTaskBackend&)> diagnostic;
    std::jthread worker;

    SimulationTaskState state_locked() const {
        if (fatal) std::rethrow_exception(fatal);
        auto state = progress;
        if (requested == Request::stop) state.status = SimulationTaskStatus::stopping;
        else if (terminal) state.status = *terminal;
        else if (requested == Request::cancel) state.status = SimulationTaskStatus::cancelling;
        else if (requested == Request::pause)
            state.status = state.initialized && !state.batch_active ? SimulationTaskStatus::paused : SimulationTaskStatus::pausing;
        else state.status = state.initialized ? SimulationTaskStatus::running : SimulationTaskStatus::initializing;
        return state;
    }
    void boundary_locked() {
        progress.batch_active = false;
        if (requested == Request::cancel) terminal = SimulationTaskStatus::cancelled;
        else if (progress.completed_steps == progress.target_steps) terminal = SimulationTaskStatus::succeeded;
    }
    void fail_locked(const Error& error) {
        progress.error = error.code; progress.batch_active = false; terminal = SimulationTaskStatus::failed;
    }
    void work(Factory factory, std::int64_t dt) noexcept {
        DK_PROFILE_THREAD_NAME("simulation worker");
        std::unique_ptr<SimulationTaskBackend> backend;
        try {
            bool initialize = false;
            {
                std::lock_guard lock{mutex};
                initialize = requested != Request::stop && requested != Request::cancel;
                progress.batch_active = initialize;
                if (!initialize) boundary_locked();
            }
            if (initialize) {
                DK_PROFILE_ZONE("simulation.task.initialize");
                backend = factory();
                if (!backend) throw std::logic_error{"Missing simulation backend"};
                const auto result = backend->initialize();
                std::lock_guard lock{mutex};
                if (!result) fail_locked(result.error());
                else { progress.initialized = true; boundary_locked(); }
            }
            for (;;) {
                std::unique_lock lock{mutex};
                changed.wait(lock,[&] { return requested == Request::stop || diagnostic || (!terminal && requested == Request::run); });
                if (requested == Request::stop) break;
                if (diagnostic) {
                    auto call = std::move(diagnostic); diagnostic = {};
                    lock.unlock(); call(*backend); continue;
                }
                const auto count = static_cast<std::uint32_t>(std::min<std::uint64_t>(progress.batch_steps,
                    progress.target_steps-progress.completed_steps));
                progress.batch_active = true; // Claim one batch under the same lock as cancel/pause.
                lock.unlock();
                DK_PROFILE_ZONE("simulation.task.batch");
                const auto submitted = backend->submit(dt,count);
                lock.lock(); progress.submitted_steps = backend->submitted();
                if (!submitted) { fail_locked(submitted.error()); continue; }
                lock.unlock();
                for (;;) {
                    const auto complete = backend->poll();
                    lock.lock();
                    if (!complete) { fail_locked(complete.error()); break; }
                    if (*complete) { progress.completed_steps = progress.submitted_steps; boundary_locked(); break; }
                    // Cancel/pause cannot retire an unfinished GPU frame. Poll without queue-idle or blocking wait.
                    changed.wait_for(lock,std::chrono::milliseconds{1});
                    lock.unlock();
                }
            }
        } catch (...) {
            std::lock_guard lock{mutex}; fatal = std::current_exception();
        }
        // ThreadContext, backend and pending frame are destroyed on their owning thread.
        backend.reset();
        { std::lock_guard lock{mutex}; finished = true; }
        changed.notify_all();
    }
    template<class T, class F> Result<T> inspect(F function) {
        std::future<Result<T>> result;
        {
            std::lock_guard lock{mutex};
            const auto state = state_locked();
            if (!state.initialized || state.error || state.batch_active || requested == Request::stop ||
                (state.status != SimulationTaskStatus::paused && state.status != SimulationTaskStatus::succeeded &&
                 state.status != SimulationTaskStatus::cancelled) || diagnostic)
                return std::unexpected(Error{ErrorCode::invalid_state,"Diagnostics require a settled healthy simulation task"});
            auto task = std::make_shared<std::packaged_task<Result<T>(SimulationTaskBackend&)>>(
                [this,function=std::move(function)](SimulationTaskBackend& backend) {
                    auto value = function(backend);
                    if (!value) { std::lock_guard guard{mutex}; fail_locked(value.error()); }
                    return value;
                });
            result = task->get_future();
            diagnostic = [task=std::move(task)](SimulationTaskBackend& backend) { (*task)(backend); };
        }
        changed.notify_all();
        return result.get();
    }
};
AsyncSimulation::AsyncSimulation(Factory factory, std::int64_t dt, std::uint32_t count, std::uint32_t batch)
    : impl_(std::make_unique<Impl>()) {
    if (!factory || !count || count > 1000000 || !batch || batch > 8 || dt < 1000000 || dt > 33333333)
        throw std::invalid_argument{"Invalid simulation task limits"};
    impl_->progress.target_steps = count; impl_->progress.batch_steps = batch;
    impl_->worker = std::jthread{[p=impl_.get(),factory=std::move(factory),dt]() mutable { p->work(std::move(factory),dt); }};
}
std::shared_ptr<AsyncSimulation> AsyncSimulation::create(XpbdSolver cpu, bool gpu, std::int64_t dt, std::uint32_t count, std::uint32_t batch) {
    return std::make_shared<AsyncSimulation>([cpu=std::move(cpu),gpu] { return std::make_unique<Backend>(cpu,gpu); },dt,count,batch);
}
AsyncSimulation::~AsyncSimulation() { stop(); impl_->worker.join(); }
SimulationTaskState AsyncSimulation::state() const { std::lock_guard lock{impl_->mutex}; return impl_->state_locked(); }
Result<void> AsyncSimulation::pause() {
    std::lock_guard lock{impl_->mutex};
    if (impl_->requested == Impl::Request::stop || impl_->requested == Impl::Request::cancel)
        return std::unexpected(Error{ErrorCode::invalid_state,"Task is cancelling or stopping"});
    if (!impl_->terminal) impl_->requested = Impl::Request::pause;
    return {};
}
Result<void> AsyncSimulation::resume() {
    std::lock_guard lock{impl_->mutex};
    if (impl_->terminal || impl_->requested == Impl::Request::stop || impl_->requested == Impl::Request::cancel)
        return std::unexpected(Error{ErrorCode::invalid_state,"Task cannot resume"});
    if (impl_->state_locked().status != SimulationTaskStatus::paused)
        return std::unexpected(Error{ErrorCode::invalid_state,"Wait for the task to become paused before resuming"});
    impl_->requested = Impl::Request::run; impl_->changed.notify_all(); return {};
}
Result<void> AsyncSimulation::cancel() {
    std::lock_guard lock{impl_->mutex};
    if (impl_->requested == Impl::Request::stop)
        return std::unexpected(Error{ErrorCode::invalid_state,"Task is stopping"});
    if (!impl_->terminal) {
        impl_->requested = Impl::Request::cancel;
        if (!impl_->progress.batch_active && impl_->progress.initialized) impl_->terminal = SimulationTaskStatus::cancelled;
    }
    impl_->changed.notify_all(); return {};
}
void AsyncSimulation::stop() noexcept {
    std::lock_guard lock{impl_->mutex}; impl_->requested = Impl::Request::stop; impl_->changed.notify_all();
}
bool AsyncSimulation::closed() const { std::lock_guard lock{impl_->mutex}; return impl_->finished; }
Result<XpbdSnapshot> AsyncSimulation::read() { return impl_->inspect<XpbdSnapshot>([](auto& backend) { return backend.read(); }); }
#ifdef DK_SIMULATION_GPU
Result<SimulationImage> AsyncSimulation::capture(std::int64_t dt,const ClothConfig& cloth,std::uint32_t width,std::uint32_t height) {
    return impl_->inspect<SimulationImage>([=](auto& backend) { return backend.capture(dt,cloth,width,height); });
}
#endif
}
}

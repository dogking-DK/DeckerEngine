#pragma once
#include <dk/physics/FixedStepClock.hpp>
#include <dk/physics/Xpbd.hpp>
#include <dk/services/SceneService.hpp>
#include <chrono>

namespace dk {
namespace graphics { class Device; }
struct SimulationIdTag;
using SimulationId = StableId<SimulationIdTag>;
enum class SimulationMode { edit, running, paused, initializing, pausing, cancelling, stopping };
enum class SimulationTaskStatus { initializing, running, pausing, paused, cancelling, cancelled, succeeded, failed, stopping };
[[nodiscard]] std::string_view simulation_task_status_name(SimulationTaskStatus) noexcept;
struct SimulationTaskState {
    SimulationTaskStatus status = SimulationTaskStatus::initializing;
    std::uint64_t target_steps = 0, submitted_steps = 0, completed_steps = 0;
    std::uint32_t batch_steps = 8;
    bool initialized = false, batch_active = false;
    std::optional<ErrorCode> error;
};
namespace detail { class GpuSimulation; class AsyncSimulation; }
struct SimulationRunState {
    SimulationId run_id;
    DocumentState source;
    FixedStepConfig config;
    FixedStepState clock;
    std::optional<ErrorCode> fault;
    std::optional<ClothConfig> cloth;
    std::optional<XpbdMetrics> metrics;
    bool gpu = false;
    std::optional<SimulationTaskState> task;
};
struct SimulationState {
    SimulationMode mode;
    std::optional<SimulationRunState> run;
};
struct PlaySceneSnapshot {
    SimulationRunState run;
    Project project;
    SceneSnapshot scene;
    std::filesystem::path manifest;
};
struct PlayParticleSnapshot {
    SimulationRunState run;
    XpbdSnapshot data;
};
struct SimulationParticlePage {
    SimulationRunState run;
    std::size_t offset, total;
    std::vector<ParticlePosition> positions;
    std::vector<ParticleVelocity> velocities;
};
// Owner-thread service. Only start/run read EditWorld; no operation writes it.
class SimulationService final {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;
    [[nodiscard]] Result<void> set_gpu_device(std::shared_ptr<const graphics::Device>);
    [[nodiscard]] Result<void> run(const SceneService&, EditGuard, std::uint32_t count,
        ClothConfig = {}, bool gpu = false, std::int64_t dt_ns = 10000000, std::uint32_t batch_steps = 8);
    [[nodiscard]] Result<void> cancel(SimulationId);
    [[nodiscard]] Result<void> start(const SceneService&, EditGuard, FixedStepConfig = {},
                                     bool paused = false, std::optional<TimePoint> now = {}, std::optional<ClothConfig> cloth = {}, bool gpu = false);
    [[nodiscard]] Result<void> pause(SimulationId);
    [[nodiscard]] Result<void> resume(SimulationId, TimePoint now = Clock::now());
    [[nodiscard]] Result<void> step(SimulationId, std::uint32_t count = 1);
    [[nodiscard]] Result<void> stop(SimulationId);
    [[nodiscard]] SimulationState state() const;
    [[nodiscard]] Result<PlaySceneSnapshot> read_snapshot(SimulationId) const;
    [[nodiscard]] Result<PlayParticleSnapshot> read_particles(SimulationId) const;
    [[nodiscard]] Result<SimulationParticlePage> particle_page(SimulationId, std::size_t offset = 0, std::size_t limit = 128) const;
#ifdef DK_SIMULATION_GPU
    // Returns the normalized project-relative directory. Never overwrites an existing target.
    [[nodiscard]] Result<std::string> export_experiment(SimulationId, std::uint64_t expected_steps,
        std::string_view output, std::uint32_t width = 640, std::uint32_t height = 480);
#endif
    void pump(TimePoint now = Clock::now());
    [[nodiscard]] TimePoint next_deadline(TimePoint fallback) const;
    void shutdown() noexcept { play_.reset(); }
    // Request now, join at service destruction so host teardown can overlap.
    void request_shutdown() noexcept;
private:
    struct PlayWorld {
        SimulationId id;
        DocumentState source;
        Project project;
        std::filesystem::path manifest;
        std::unique_ptr<SceneDocument> scene;
        FixedStepClock clock;
        std::optional<ClothConfig> cloth;
        std::optional<XpbdSolver> solver;
        bool paused;
        TimePoint last_pump;
        std::optional<ErrorCode> fault;
        std::shared_ptr<detail::GpuSimulation> gpu;
        std::shared_ptr<detail::AsyncSimulation> task;
        bool task_gpu = false;
        std::optional<SimulationTaskState> progress;
    };
    [[nodiscard]] Result<void> check_run(SimulationId) const;
    [[nodiscard]] SimulationRunState run_state() const;
    [[nodiscard]] Result<void> advance_solver(const FixedStepClock&, std::uint32_t);
    std::shared_ptr<const graphics::Device> gpu_device_;
    std::unique_ptr<PlayWorld> play_; // Join before releasing the host's device lifetime.
};
}

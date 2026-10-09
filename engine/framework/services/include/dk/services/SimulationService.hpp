#pragma once
#include <dk/physics/FixedStepClock.hpp>
#include <dk/physics/Xpbd.hpp>
#include <dk/services/SceneService.hpp>
#include <chrono>

namespace dk {
struct SimulationIdTag;
using SimulationId = StableId<SimulationIdTag>;
enum class SimulationMode { edit, running, paused };
struct SimulationRunState {
    SimulationId run_id;
    DocumentState source;
    FixedStepConfig config;
    FixedStepState clock;
    std::optional<ErrorCode> fault;
    std::optional<ClothConfig> cloth;
    std::optional<XpbdMetrics> metrics;
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
// Owner-thread service. Only start reads EditWorld; no operation writes it.
class SimulationService final {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;
    [[nodiscard]] Result<void> start(const SceneService&, EditGuard, FixedStepConfig = {},
                                     bool paused = false, std::optional<TimePoint> now = {}, std::optional<ClothConfig> cloth = {});
    [[nodiscard]] Result<void> pause(SimulationId);
    [[nodiscard]] Result<void> resume(SimulationId, TimePoint now = Clock::now());
    [[nodiscard]] Result<void> step(SimulationId, std::uint32_t count = 1);
    [[nodiscard]] Result<void> stop(SimulationId);
    [[nodiscard]] SimulationState state() const;
    [[nodiscard]] Result<PlaySceneSnapshot> read_snapshot(SimulationId) const;
    [[nodiscard]] Result<PlayParticleSnapshot> read_particles(SimulationId) const;
    [[nodiscard]] Result<SimulationParticlePage> particle_page(SimulationId, std::size_t offset = 0, std::size_t limit = 128) const;
    void pump(TimePoint now = Clock::now());
    [[nodiscard]] TimePoint next_deadline(TimePoint fallback) const;
    void shutdown() noexcept { play_.reset(); }
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
    };
    [[nodiscard]] Result<void> check_run(SimulationId) const;
    [[nodiscard]] SimulationRunState run_state() const;
    std::unique_ptr<PlayWorld> play_;
};
}

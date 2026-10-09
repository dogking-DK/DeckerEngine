#pragma once
#include <dk/services/SimulationService.hpp>
#include <functional>
#ifdef DK_SIMULATION_GPU
#include "GpuSimulation.hpp"
#endif

namespace dk::detail {
// A single worker owns the backend from initialize through destruction. Test doubles
// control the same submit/poll boundary without replacing engine scheduling code.
class SimulationTaskBackend {
public:
    virtual ~SimulationTaskBackend() = default;
    virtual Result<void> initialize() = 0;
    virtual Result<void> submit(std::int64_t, std::uint32_t) = 0;
    virtual Result<bool> poll() = 0;
    virtual std::uint64_t submitted() const = 0;
    virtual Result<XpbdSnapshot> read() = 0;
#ifdef DK_SIMULATION_GPU
    virtual Result<SimulationImage> capture(std::int64_t, const ClothConfig&, std::uint32_t, std::uint32_t) {
        return std::unexpected(Error{ErrorCode::not_supported,"Backend does not support images"});
    }
#endif
};
class AsyncSimulation final {
public:
    using Factory = std::function<std::unique_ptr<SimulationTaskBackend>()>;
    static std::shared_ptr<AsyncSimulation> create(XpbdSolver, bool gpu, std::int64_t dt, std::uint32_t count, std::uint32_t batch);
    AsyncSimulation(Factory, std::int64_t dt, std::uint32_t count, std::uint32_t batch);
    ~AsyncSimulation();
    AsyncSimulation(const AsyncSimulation&) = delete;
    AsyncSimulation& operator=(const AsyncSimulation&) = delete;
    SimulationTaskState state() const; // Owner publishes this snapshot, including fatal exceptions.
    Result<void> pause();
    Result<void> resume();
    Result<void> cancel();
    void stop() noexcept;
    bool closed() const;
    Result<XpbdSnapshot> read();
#ifdef DK_SIMULATION_GPU
    Result<SimulationImage> capture(std::int64_t, const ClothConfig&, std::uint32_t, std::uint32_t);
#endif
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}

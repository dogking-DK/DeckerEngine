#pragma once
#include <dk/physics/GpuXpbd.hpp>

namespace dk::detail {
struct SimulationImage {
    GpuParticles particles;
    std::vector<std::byte> rgba;
};
// Private owner-thread backend. All waits bound outstanding submissions, not CPU readbacks.
class GpuSimulation final {
public:
    static Result<std::shared_ptr<GpuSimulation>> create(const XpbdSolver&);
    ~GpuSimulation();
    Result<void> step(std::int64_t dt, std::uint32_t count);
    Result<GpuParticles> read(std::int64_t dt);
    Result<SimulationImage> capture(std::int64_t dt, const ClothConfig&, std::uint32_t width, std::uint32_t height);
    std::uint64_t steps() const;
private:
    struct Impl;
    explicit GpuSimulation(std::unique_ptr<Impl>);
    std::unique_ptr<Impl> impl_;
};
}

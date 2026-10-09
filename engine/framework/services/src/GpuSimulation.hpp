#pragma once
#include <dk/physics/GpuXpbd.hpp>
#include <dk/services/SimulationPreview.hpp>

namespace dk::detail {
struct SimulationImage {
    GpuParticles particles;
    std::vector<std::byte> rgba;
};
// Private backend, used and destroyed on its creation thread. Finite tasks use submit/poll;
// synchronous legacy steps and explicit readbacks retain their wait boundary.
class GpuSimulation final {
public:
    // Successful null means cooperative cancellation before initialization publication.
    static Result<std::shared_ptr<GpuSimulation>> create(const XpbdSolver&, std::stop_token = {},
        std::shared_ptr<const graphics::Device> device={}, bool renderer_only=false);
    ~GpuSimulation();
    Result<void> step(std::int64_t dt, std::uint32_t count);
    Result<bool> submit(std::int64_t dt, std::uint32_t count, std::stop_token);
    Result<bool> poll();
    Result<GpuParticles> read(std::int64_t dt);
    Result<SimulationImage> capture(std::int64_t dt, const ClothConfig&, std::uint32_t width, std::uint32_t height);
    Result<graphics::ImageTransfer> preview(std::int64_t dt, const ClothConfig&, const SimulationView&,
        std::span<const ParticlePosition> cpu_positions = {});
    std::uint64_t steps() const;
private:
    struct Impl;
    explicit GpuSimulation(std::unique_ptr<Impl>);
    std::unique_ptr<Impl> impl_;
};
}

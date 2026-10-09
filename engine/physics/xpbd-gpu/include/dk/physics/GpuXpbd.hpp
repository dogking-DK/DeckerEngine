#pragma once
#include <dk/physics/Xpbd.hpp>
#include <dk/graphics/GraphExecution.hpp>
#include <filesystem>
#include <stop_token>

namespace dk {
namespace detail { struct GpuXpbdState; struct GpuXpbdFrameState; }
struct GpuXpbdOptions { bool readback = false, capture_plan = false; std::stop_token cancel; };
struct GpuParticles {
    std::vector<ParticlePosition> positions;
    std::vector<ParticleVelocity> velocities;
};
// Borrowed only during advance. Append read-only consumers of positions; never submit here.
// All solver buffers are declared before this callback; no solver images are declared.
struct GpuParticleGraph {
    graphics::graph::Graph& graph;
    graphics::graph::BufferId positions;
    std::size_t position_resource;
    std::uint32_t particle_count;
    std::vector<graphics::graph::PassCallback>& callbacks;
    std::vector<graphics::graph::FinalAccess>& final_accesses;
};
struct GpuParticleConsumer {
    Result<void> (*append)(GpuParticleGraph&, void*) = nullptr;
    void* context = nullptr; // Must outlive advance, including synchronous recording.
};
class GpuXpbdFrame final {
public:
    GpuXpbdFrame() = default;
    [[nodiscard]] explicit operator bool() const noexcept { return bool(state_); }
    [[nodiscard]] std::uint64_t steps() const noexcept;
    [[nodiscard]] graphics::Submission submission() const noexcept;
    [[nodiscard]] Result<bool> wait(graphics::SubmissionQueue&, std::uint64_t timeout_ns = UINT64_MAX) const;
    [[nodiscard]] Result<GpuParticles> read_particles() const;
    [[nodiscard]] const graphics::graph::Execution* execution() const noexcept;
    [[nodiscard]] std::string_view plan_text() const noexcept;
private:
    friend class GpuXpbdSolver;
    explicit GpuXpbdFrame(std::shared_ptr<const detail::GpuXpbdFrameState> state) : state_(std::move(state)) {}
    std::shared_ptr<const detail::GpuXpbdFrameState> state_;
};
// Externally serialized, single queue. Submitted frames remain immutable across later advances.
class GpuXpbdSolver final {
public:
    GpuXpbdSolver() = default;
    GpuXpbdSolver(GpuXpbdSolver&&) noexcept = default;
    GpuXpbdSolver& operator=(GpuXpbdSolver&&) noexcept = default;
    GpuXpbdSolver(const GpuXpbdSolver&) = delete;
    GpuXpbdSolver& operator=(const GpuXpbdSolver&) = delete;
    [[nodiscard]] static Result<GpuXpbdSolver> create(memory::ResourceHandle, graphics::SubmissionQueue&,
        const XpbdSolver&, const std::filesystem::path& shader_file, std::stop_token = {});
    // count 0..8; 0 produces a display-only frame. Success means submitted, not completed/finite.
    [[nodiscard]] Result<GpuXpbdFrame> advance(graphics::SubmissionQueue&, std::int64_t fixed_dt_ns,
        std::uint32_t count = 1, GpuXpbdOptions = {}, GpuParticleConsumer = {});
    [[nodiscard]] std::uint64_t steps() const noexcept;
    [[nodiscard]] std::uint32_t particle_count() const noexcept;
private:
    explicit GpuXpbdSolver(std::shared_ptr<detail::GpuXpbdState> state) : state_(std::move(state)) {}
    std::shared_ptr<detail::GpuXpbdState> state_;
};
}

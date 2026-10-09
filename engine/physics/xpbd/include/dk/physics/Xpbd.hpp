#pragma once
#include <dk/core/Result.hpp>
#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace dk {
struct alignas(16) ParticlePosition {
    float x = 0, y = 0, z = 0, inverse_mass = 1;
    bool operator==(const ParticlePosition&) const = default;
};
struct alignas(16) ParticleVelocity {
    float x = 0, y = 0, z = 0, padding = 0;
    bool operator==(const ParticleVelocity&) const = default;
};
struct alignas(16) DistanceConstraint {
    std::uint32_t a, b;
    float rest_length, compliance;
    bool operator==(const DistanceConstraint&) const = default;
};
static_assert(sizeof(ParticlePosition) == 16 && sizeof(ParticleVelocity) == 16 && sizeof(DistanceConstraint) == 16);
struct XpbdConfig {
    std::uint32_t iterations = 12;
    float gravity_y = -9.81f, floor_y = 0, damping = 0.5f;
};
struct ClothConfig {
    std::uint32_t columns = 8, rows = 8, seed = 1;
    float spacing = 0.15f, height = 0.75f, particle_mass = 0.1f, compliance = 1e-6f;
    XpbdConfig physics;
};
struct XpbdMetrics {
    std::uint32_t particle_count = 0, constraint_count = 0, color_count = 0;
    double max_constraint_error = 0, rms_constraint_error = 0, max_relative_error = 0;
    double max_speed = 0, kinetic_energy = 0, gravity_potential_energy = 0, compliant_energy = 0;
    double min_height = 0, max_penetration = 0, max_pin_displacement = 0;
    bool operator==(const XpbdMetrics&) const = default;
};
struct XpbdSnapshot {
    XpbdConfig config;
    std::vector<ParticlePosition> positions;
    std::vector<ParticleVelocity> velocities;
    std::vector<DistanceConstraint> constraints;
    std::vector<std::uint32_t> color_offsets;
    XpbdMetrics metrics;
    std::vector<std::array<float, 3>> initial_directions;
};
// Single-owner CPU reference. No scene entities, device objects or filesystem access.
class XpbdSolver final {
public:
    [[nodiscard]] static Result<XpbdSolver> create(XpbdConfig, std::vector<ParticlePosition>,
        std::vector<ParticleVelocity>, std::vector<DistanceConstraint>);
    [[nodiscard]] static Result<XpbdSolver> cloth(ClothConfig = {});
    // All N steps commit together; any reported failure preserves arrays and metrics.
    [[nodiscard]] Result<void> advance(std::int64_t fixed_dt_ns, std::uint32_t count = 1);
    [[nodiscard]] XpbdSnapshot snapshot() const;
    [[nodiscard]] const XpbdMetrics& metrics() const noexcept { return metrics_; }
    // Views expire at the next successful advance or destruction.
    [[nodiscard]] std::span<const ParticlePosition> positions() const noexcept { return positions_; }
    [[nodiscard]] std::span<const ParticleVelocity> velocities() const noexcept { return velocities_; }
private:
    XpbdSolver() = default;
    [[nodiscard]] XpbdMetrics measure(std::span<const ParticlePosition>, std::span<const ParticleVelocity>) const;
    XpbdConfig config_;
    std::vector<ParticlePosition> positions_, initial_;
    std::vector<ParticleVelocity> velocities_;
    std::vector<DistanceConstraint> constraints_;
    std::vector<std::array<float, 3>> directions_;
    std::vector<std::uint32_t> color_offsets_;
    XpbdMetrics metrics_;
};
}

#pragma once
#include <dk/math/Types.hpp>
#include <dk/render/CloudFields.hpp>
#include <stop_token>

namespace dk::render {
struct PathMediumSettings {
    std::uint32_t model_version = 1;
    double time_seconds = 0, cloud_albedo = .9, cloud_anisotropy = .6;
    bool operator==(const PathMediumSettings&) const = default;
};
struct PathMediumCoefficients {
    Vec3d extinction = Vec3d::Zero();
    Vec3d rayleigh_scattering = Vec3d::Zero(), mie_scattering = Vec3d::Zero();
    double cloud_scattering = 0; // Achromatic; already multiplied by albedo.
};
struct PathMediumRay {
    Vec3d origin = Vec3d::Zero(), direction = Vec3d::UnitY();
    double distance_m = 1000000;
};
struct PathMediumSegment { double begin_m = 0, end_m = 0; bool clouds = false; };
struct PathMediumSegments {
    std::array<PathMediumSegment,5> segments{};
    std::uint32_t count = 0;
    bool ground = false;
    double ground_distance_m = 0; // Valid only when ground is true.
};
struct PathMediumIntegration {
    std::uint32_t steps = 512; // Per segment, 1..8192.
    std::uint64_t max_samples = 40960;
};
struct PathMediumTransmittance {
    Vec3d transmittance = Vec3d::Ones(), optical_depth = Vec3d::Zero();
    std::uint64_t samples = 0;
    bool ground = false;
};
// Immutable world-space inputs, independent of views, light integration and GPU state.
// The caller owns field preparation/budgets; a snapshot retains only an existing field.
class PathMediumSnapshot final {
public:
    PathMediumSnapshot() = default;
    [[nodiscard]] static Result<PathMediumSnapshot> create(const EnvironmentDescription&,
        const PathMediumSettings& = {}, std::shared_ptr<const CloudFieldData> = {});
    [[nodiscard]] explicit operator bool() const noexcept { return valid_; }
    [[nodiscard]] const EnvironmentDescription& environment() const noexcept { return environment_; }
    [[nodiscard]] const PathMediumSettings& settings() const noexcept { return settings_; }
    [[nodiscard]] std::uint64_t field_hash() const noexcept { return field_ ? field_->hash() : 0; }
    [[nodiscard]] Vec3d extinction_majorant() const noexcept { return majorant_; }
    [[nodiscard]] Result<PathMediumCoefficients> sample(const Vec3d& position_m) const;
    [[nodiscard]] Result<PathMediumSegments> intervals(const PathMediumRay&) const;
    // T is only medium transmission up to ground. Ground opacity is a separate boundary.
    // No partial result is returned after cancellation or a work-budget error.
    [[nodiscard]] Result<PathMediumTransmittance> integrate(const PathMediumRay&,
        const PathMediumIntegration& = {}, std::stop_token = {}) const;
private:
    [[nodiscard]] Result<PathMediumCoefficients> sample_at(const Vec3d&, double height_m) const;
    EnvironmentDescription environment_;
    PathMediumSettings settings_;
    std::shared_ptr<const CloudFieldData> field_;
    Vec3d majorant_ = Vec3d::Zero();
    bool valid_ = false, clouds_ = false;
};
} // namespace dk::render

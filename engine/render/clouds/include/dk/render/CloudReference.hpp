#pragma once
#include <dk/render/CloudFields.hpp>
#include <dk/render/AtmosphereReference.hpp>

namespace dk::render {
struct CloudRay {
    Vec3d origin_m{0,0,0}, direction{0,1,0};
    double max_distance_m=1000000;
};
struct CloudInterval { double begin_m=0, end_m=0; };
struct CloudIntervals {
    std::array<CloudInterval,2> segments{};
    std::uint32_t count=0;
    bool ground=false;
};
struct CloudReferenceSettings {
    std::uint32_t model_version=1, steps=256, sun_steps=64, atmosphere_steps=64;
    double time_seconds=0, phase_g=.6, single_scattering_albedo=.9, multiple_scattering=.35;
    Vec3d sky_radiance=Vec3d::Zero(); // Explicit isotropic boundary approximation, before exposure.
    bool self_shadow=true, homogeneous=false;
    double homogeneous_density=1; // [0,1] times clouds.density; diagnostic shell, no noise/profile.
    std::uint64_t max_work=256*1024*1024;
    std::size_t max_bytes=4*1024*1024; // GPU batch input/output/readback logical payload.
    bool operator==(const CloudReferenceSettings&) const = default;
};
struct CloudIntegral {
    Vec3d radiance=Vec3d::Zero();
    double transmittance=1, distance_m=0, mean_distance_m=0;
    std::uint32_t samples=0;
};
struct CloudCompositionSettings {
    CloudReferenceSettings reference;
    bool surface_lighting=true, sun_disk=true, diagnostics=false;
    std::uint64_t max_work=1024ULL*1024*1024; // <=4096 pixels, at most five medium segments.
    bool operator==(const CloudCompositionSettings&) const = default;
};
struct CloudOptimizedSettings {
    CloudCompositionSettings composition=[] { CloudCompositionSettings s; s.reference.steps=96; s.reference.sun_steps=8; return s; }();
    std::uint64_t view_id=1, camera_revision=0; // Stable view identity; change camera_revision on camera selection/cut.
    bool temporal=true, empty_space_skipping=true;
    float termination_transmittance=1e-4f, history_weight=.85f;
    std::uint64_t max_work=128ULL*1024*1024*1024; // Half image plus worst-case full-resolution fallback.
    bool operator==(const CloudOptimizedSettings&) const = default;
};
struct CloudMediumIntegral {
    Vec3d radiance=Vec3d::Zero(), transmittance=Vec3d::Ones(), cloud_radiance=Vec3d::Zero();
    double cloud_transmittance=1, mean_cloud_distance_m=0;
    bool ground=false;
};
// Image reference bounds; validation does not allocate GPU resources.
[[nodiscard]] Result<void> validate_cloud_composition(const EnvironmentDescription&, const CloudCompositionSettings&, std::size_t pixels);
[[nodiscard]] Result<void> validate_cloud_optimization(const EnvironmentDescription&, const CloudOptimizedSettings&,
    std::uint32_t width, std::uint32_t height);
// Joint view-path integration; air uses single scattering, cloud uses the explicit
// reference approximation. Terminal surface/solar radiance is added by the caller.
[[nodiscard]] Result<CloudMediumIntegral> integrate_cloud_medium_reference(const EnvironmentDescription&, const CloudFieldData&,
    const CloudRay&, const CloudReferenceSettings& = {});
// Ordered positive shell segments, clipped at ground and the finite ray limit.
[[nodiscard]] Result<CloudIntervals> cloud_shell_intervals(const EnvironmentDescription&, const CloudRay&);
// Pure double-precision SI reference. No cache mutation or implicit time/history.
[[nodiscard]] Result<CloudIntegral> integrate_cloud_reference(const EnvironmentDescription&, const CloudFieldData&,
    const CloudRay&, const CloudReferenceSettings& = {});
} // namespace dk::render

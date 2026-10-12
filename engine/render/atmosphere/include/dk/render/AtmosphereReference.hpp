#pragma once
#include <dk/scene/Environment.hpp>
#include <dk/math/Types.hpp>
#include <limits>

namespace dk::render {
struct AtmosphereReferenceSettings {
    std::uint32_t steps=512, sun_steps=256;
    double max_distance_m=std::numeric_limits<double>::infinity();
    bool sun_disk=false; // Requires infinite max_distance_m; disk is a distant boundary source.
};
struct AtmosphereIntegral {
    Vec3d radiance=Vec3d::Zero(), transmittance=Vec3d::Ones();
    double distance_m=0;
    bool ground=false;
};
struct MultipleScatteringReference {
    Vec3d radiance=Vec3d::Zero(), feedback=Vec3d::Zero();
};
// Phase arguments: cosine in [-1,1], anisotropy in (-1,1).
[[nodiscard]] double rayleigh_phase(double cosine) noexcept;
[[nodiscard]] double mie_phase(double cosine, double anisotropy) noexcept;
// Independent double-precision SI reference: no Vulkan, textures or compiler.
// Positions use Scene limits (+/-1e9 m); direct height input is limited to 4e9 m.
[[nodiscard]] Result<AtmosphereIntegral> integrate_atmosphere_reference(const EnvironmentDescription&,
    const Vec3d& position_m, const Vec3d& direction, const AtmosphereReferenceSettings& = {});
[[nodiscard]] Result<Vec3d> atmosphere_transmittance_reference(const AtmosphereParameters&, double height_m,
    double zenith_cosine, std::uint32_t steps=512);
// Unit solar irradiance; isotropic closure, not a path-traced all-orders reference.
[[nodiscard]] Result<MultipleScatteringReference> multiple_scattering_reference(const AtmosphereParameters&,
    double height_m, double sun_cosine, std::uint32_t direction_side=16, std::uint32_t steps=128, std::uint32_t sun_steps=256);
} // namespace dk::render

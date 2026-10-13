#pragma once
#include <dk/core/Result.hpp>
#include <array>
#include <cstdint>

namespace dk {
using EnvironmentVector = std::array<double, 3>;

// SI lengths and inverse lengths; RGB is linear sRGB/D65, before exposure.
struct AtmosphereParameters {
    EnvironmentVector planet_center_m{0.0, -6360000.0, 0.0};
    double ground_radius_m = 6360000.0;
    double atmosphere_height_m = 100000.0;
    EnvironmentVector rayleigh_scattering_per_m{5.802e-6, 13.558e-6, 33.1e-6};
    double rayleigh_scale_height_m = 8000.0;
    EnvironmentVector mie_scattering_per_m{3.996e-6, 3.996e-6, 3.996e-6};
    EnvironmentVector mie_extinction_per_m{4.44e-6, 4.44e-6, 4.44e-6};
    double mie_scale_height_m = 1200.0;
    double mie_anisotropy = 0.8;
    EnvironmentVector absorption_extinction_per_m{0.65e-6, 1.881e-6, 0.085e-6};
    double absorption_center_height_m = 25000.0;
    double absorption_half_width_m = 15000.0;
    EnvironmentVector ground_albedo{0.3, 0.3, 0.3};
    bool operator==(const AtmosphereParameters&) const = default;
};
struct SunParameters {
    EnvironmentVector direction{0.0, 1.0, 0.0}; // From sample point toward sun.
    EnvironmentVector irradiance{1.0, 1.0, 1.0}; // Normal irradiance in scene units, not lux.
    double angular_radius_rad = 0.004675;
    bool operator==(const SunParameters&) const = default;
};
struct CloudParameters {
    bool enabled = false;
    std::uint32_t seed = 1;
    std::uint32_t generator_version = 1;
    std::uint32_t weather_resolution = 128, base_resolution = 128, detail_resolution = 32;
    double bottom_height_m = 1500, top_height_m = 4000;
    double coverage = 0.5, density = 1, extinction_per_m = 0.001;
    double weather_scale_m = 100000, shape_scale_m = 10000, detail_scale_m = 1000;
    double bottom_fade_fraction = 0.1, top_fade_fraction = 0.2;
    std::array<double, 2> wind_velocity_mps{10, 0}; // World X/Z, evaluated at explicit time.
    bool operator==(const CloudParameters&) const = default;
};
struct EnvironmentDescription {
    bool enabled = false;
    AtmosphereParameters atmosphere;
    SunParameters sun;
    CloudParameters clouds;
    bool operator==(const EnvironmentDescription&) const = default;
};
// Validates all fields even when disabled; preserves already-unit directions.
[[nodiscard]] Result<EnvironmentDescription> normalize_environment(EnvironmentDescription);
[[nodiscard]] Result<void> validate_clouds(const CloudParameters&, double atmosphere_height_m);
// Signed spherical height, including below ground and above atmosphere top.
[[nodiscard]] Result<double> atmosphere_height_m(const AtmosphereParameters&, const EnvironmentVector& position_m);
// Uniform disk L = E / (pi sin(alpha)^2), in the same scene-linear units as emissive.
[[nodiscard]] Result<EnvironmentVector> solar_disk_radiance(const SunParameters&);
} // namespace dk

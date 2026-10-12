#include <dk/scene/Environment.hpp>
#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

namespace dk {
namespace {
bool range(double value, double low, double high) {
    return std::isfinite(value) && value >= low && value <= high;
}
bool range(const EnvironmentVector& value, double low, double high) {
    return std::ranges::all_of(value, [=](double v) { return range(v, low, high); });
}
Result<void> validate_atmosphere(const AtmosphereParameters& a) {
    if (!range(a.planet_center_m, -1e9, 1e9) || !range(a.ground_radius_m, 1, 1e9) ||
        !range(a.atmosphere_height_m, 1, 1e7) ||
        !range(a.rayleigh_scattering_per_m, 0, 1) || !range(a.mie_scattering_per_m, 0, 1) ||
        !range(a.mie_extinction_per_m, 0, 1) || !range(a.absorption_extinction_per_m, 0, 1) ||
        !range(a.ground_albedo, 0, 1) || !range(a.mie_anisotropy, -0.99, 0.99) ||
        !range(a.rayleigh_scale_height_m, 1, a.atmosphere_height_m) ||
        !range(a.mie_scale_height_m, 1, a.atmosphere_height_m) ||
        !range(a.absorption_center_height_m, 0, a.atmosphere_height_m) ||
        !range(a.absorption_half_width_m, 1, a.atmosphere_height_m))
        return std::unexpected(Error{ErrorCode::invalid_argument, "Atmosphere parameter outside supported finite range"});
    for (std::size_t i = 0; i < 3; ++i)
        if (a.mie_scattering_per_m[i] > a.mie_extinction_per_m[i])
            return std::unexpected(Error{ErrorCode::invalid_argument, "Mie scattering exceeds extinction"});
    return {};
}
Result<SunParameters> normalize_sun(SunParameters sun) {
    if (!range(sun.irradiance, 0, 1e6) || !range(sun.angular_radius_rad, 1e-6, 0.1) ||
        !std::ranges::all_of(sun.direction, [](double v) { return std::isfinite(v); }))
        return std::unexpected(Error{ErrorCode::invalid_argument, "Invalid sun irradiance, radius or direction"});
    const double scale = std::max({std::abs(sun.direction[0]), std::abs(sun.direction[1]), std::abs(sun.direction[2])});
    if (scale == 0)
        return std::unexpected(Error{ErrorCode::invalid_argument, "Sun direction must be nonzero"});
    // Rescale first so finite huge and subnormal inputs neither overflow nor underflow.
    const EnvironmentVector scaled{sun.direction[0] / scale, sun.direction[1] / scale, sun.direction[2] / scale};
    const double length = std::hypot(scaled[0], scaled[1], scaled[2]);
    if (std::abs(scale * length - 1) > 8 * std::numeric_limits<double>::epsilon())
        for (std::size_t i = 0; i < 3; ++i) sun.direction[i] = scaled[i] / length;
    return sun;
}
}
Result<EnvironmentDescription> normalize_environment(EnvironmentDescription value) {
    auto atmosphere = validate_atmosphere(value.atmosphere);
    if (!atmosphere) return std::unexpected(atmosphere.error());
    auto sun = normalize_sun(value.sun);
    if (!sun) return std::unexpected(sun.error());
    value.sun = *sun;
    return value;
}
Result<double> atmosphere_height_m(const AtmosphereParameters& a, const EnvironmentVector& position) {
    auto valid = validate_atmosphere(a);
    if (!valid) return std::unexpected(valid.error());
    if (!range(position, -1e9, 1e9))
        return std::unexpected(Error{ErrorCode::invalid_argument, "Atmosphere position outside supported finite range"});
    return std::hypot(position[0] - a.planet_center_m[0], position[1] - a.planet_center_m[1],
        position[2] - a.planet_center_m[2]) - a.ground_radius_m;
}
Result<EnvironmentVector> solar_disk_radiance(const SunParameters& sun) {
    auto valid = normalize_sun(sun);
    if (!valid) return std::unexpected(valid.error());
    const double sine = std::sin(sun.angular_radius_rad);
    const double factor = 1 / (std::numbers::pi * sine * sine);
    return EnvironmentVector{sun.irradiance[0] * factor, sun.irradiance[1] * factor, sun.irradiance[2] * factor};
}
} // namespace dk

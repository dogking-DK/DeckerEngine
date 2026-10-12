#pragma once
#include <dk/scene/Environment.hpp>
#include "JsonSupport.hpp"
#include <cmath>
namespace dk::detail {
inline Json environment_json(const EnvironmentDescription& e) {
    return {{"version", 1}, {"enabled", e.enabled},
        {"atmosphere", {{"planet_center_m", e.atmosphere.planet_center_m},
        {"ground_radius_m", e.atmosphere.ground_radius_m},
        {"atmosphere_height_m", e.atmosphere.atmosphere_height_m},
        {"rayleigh_scattering_per_m", e.atmosphere.rayleigh_scattering_per_m},
        {"rayleigh_scale_height_m", e.atmosphere.rayleigh_scale_height_m},
        {"mie_scattering_per_m", e.atmosphere.mie_scattering_per_m},
        {"mie_extinction_per_m", e.atmosphere.mie_extinction_per_m},
        {"mie_scale_height_m", e.atmosphere.mie_scale_height_m},
        {"mie_anisotropy", e.atmosphere.mie_anisotropy},
        {"absorption_extinction_per_m", e.atmosphere.absorption_extinction_per_m},
        {"absorption_center_height_m", e.atmosphere.absorption_center_height_m},
        {"absorption_half_width_m", e.atmosphere.absorption_half_width_m},
        {"ground_albedo", e.atmosphere.ground_albedo}}},
        {"sun", {{"direction", e.sun.direction},
        {"irradiance", e.sun.irradiance},
        {"angular_radius_rad", e.sun.angular_radius_rad}}}};
}

inline double environment_number(const Json& j) {
    require(j.is_number(), "Environment value must be numeric");
    const auto value = j.get<double>();
    require(std::isfinite(value), "Environment value must be finite");
    return value;
}
inline EnvironmentVector environment_vector(const Json& j) {
    require(j.is_array() && j.size() == 3, "Environment vector must contain three numbers");
    return {environment_number(j[0]), environment_number(j[1]), environment_number(j[2])};
}
inline EnvironmentDescription read_environment(const Json& j) {
    fields(j, {"version", "enabled", "atmosphere", "sun"});
    version_one(j.at("version"));
    require(j.at("enabled").is_boolean(), "Environment enabled must be boolean");
    EnvironmentDescription e;
    e.enabled = j.at("enabled").get<bool>();
    const auto& atmosphere = j.at("atmosphere");
    fields(atmosphere, {"planet_center_m", "ground_radius_m", "atmosphere_height_m", "rayleigh_scattering_per_m", "rayleigh_scale_height_m", "mie_scattering_per_m", "mie_extinction_per_m", "mie_scale_height_m", "mie_anisotropy", "absorption_extinction_per_m", "absorption_center_height_m", "absorption_half_width_m", "ground_albedo"});
    e.atmosphere.planet_center_m = environment_vector(atmosphere.at("planet_center_m"));
    e.atmosphere.ground_radius_m = environment_number(atmosphere.at("ground_radius_m"));
    e.atmosphere.atmosphere_height_m = environment_number(atmosphere.at("atmosphere_height_m"));
    e.atmosphere.rayleigh_scattering_per_m = environment_vector(atmosphere.at("rayleigh_scattering_per_m"));
    e.atmosphere.rayleigh_scale_height_m = environment_number(atmosphere.at("rayleigh_scale_height_m"));
    e.atmosphere.mie_scattering_per_m = environment_vector(atmosphere.at("mie_scattering_per_m"));
    e.atmosphere.mie_extinction_per_m = environment_vector(atmosphere.at("mie_extinction_per_m"));
    e.atmosphere.mie_scale_height_m = environment_number(atmosphere.at("mie_scale_height_m"));
    e.atmosphere.mie_anisotropy = environment_number(atmosphere.at("mie_anisotropy"));
    e.atmosphere.absorption_extinction_per_m = environment_vector(atmosphere.at("absorption_extinction_per_m"));
    e.atmosphere.absorption_center_height_m = environment_number(atmosphere.at("absorption_center_height_m"));
    e.atmosphere.absorption_half_width_m = environment_number(atmosphere.at("absorption_half_width_m"));
    e.atmosphere.ground_albedo = environment_vector(atmosphere.at("ground_albedo"));
    const auto& sun = j.at("sun");
    fields(sun, {"direction", "irradiance", "angular_radius_rad"});
    e.sun.direction = environment_vector(sun.at("direction"));
    e.sun.irradiance = environment_vector(sun.at("irradiance"));
    e.sun.angular_radius_rad = environment_number(sun.at("angular_radius_rad"));
    return e;
}
} // namespace dk::detail

#pragma once
#include <dk/scene/Environment.hpp>
#include "JsonSupport.hpp"
#include <cmath>
#include <algorithm>
namespace dk::detail {
inline Json environment_json(const EnvironmentDescription& e) {
    return {{"version", 2}, {"enabled", e.enabled},
        {"clouds", {{"enabled", e.clouds.enabled},
            {"seed", e.clouds.seed},
            {"generator_version", e.clouds.generator_version},
            {"weather_resolution", e.clouds.weather_resolution},
            {"base_resolution", e.clouds.base_resolution},
            {"detail_resolution", e.clouds.detail_resolution},
            {"bottom_height_m", e.clouds.bottom_height_m},
            {"top_height_m", e.clouds.top_height_m},
            {"coverage", e.clouds.coverage},
            {"density", e.clouds.density},
            {"extinction_per_m", e.clouds.extinction_per_m},
            {"weather_scale_m", e.clouds.weather_scale_m},
            {"shape_scale_m", e.clouds.shape_scale_m},
            {"detail_scale_m", e.clouds.detail_scale_m},
            {"bottom_fade_fraction", e.clouds.bottom_fade_fraction},
            {"top_fade_fraction", e.clouds.top_fade_fraction},
            {"wind_velocity_mps", e.clouds.wind_velocity_mps}}},
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
    require(j.is_object() && j.contains("version") && j.at("version").is_number_integer(), "Environment version must be integer");
    const bool legacy = j.at("version") == 1;
    require(legacy || j.at("version") == 2, "Unsupported environment version", ErrorCode::not_supported);
    if (legacy) fields(j, {"version", "enabled", "atmosphere", "sun"});
    else fields(j, {"version", "enabled", "atmosphere", "sun", "clouds"});
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
    if (legacy) {
        // Old valid atmospheres may be smaller than the new default cloud shell.
        e.clouds.top_height_m = std::min(e.clouds.top_height_m, e.atmosphere.atmosphere_height_m * .5);
        e.clouds.bottom_height_m = std::min(e.clouds.bottom_height_m, e.clouds.top_height_m * .5);
    } else {
        const auto& c = j.at("clouds");
        fields(c, {"enabled", "seed", "generator_version", "weather_resolution", "base_resolution", "detail_resolution", "bottom_height_m", "top_height_m", "coverage", "density", "extinction_per_m", "weather_scale_m", "shape_scale_m", "detail_scale_m", "bottom_fade_fraction", "top_fade_fraction", "wind_velocity_mps"});
        require(c.at("enabled").is_boolean(), "Cloud enabled must be boolean");
        e.clouds.enabled = c.at("enabled").get<bool>();
        require(c.at("seed").is_number_integer() && c.at("seed") >= 0 && c.at("seed") <= 4294967295ULL, "Invalid cloud integer");
        e.clouds.seed = c.at("seed").get<std::uint32_t>();
        require(c.at("generator_version").is_number_integer() && c.at("generator_version") >= 0 && c.at("generator_version") <= 4294967295ULL, "Invalid cloud integer");
        e.clouds.generator_version = c.at("generator_version").get<std::uint32_t>();
        require(c.at("weather_resolution").is_number_integer() && c.at("weather_resolution") >= 0 && c.at("weather_resolution") <= 4294967295ULL, "Invalid cloud integer");
        e.clouds.weather_resolution = c.at("weather_resolution").get<std::uint32_t>();
        require(c.at("base_resolution").is_number_integer() && c.at("base_resolution") >= 0 && c.at("base_resolution") <= 4294967295ULL, "Invalid cloud integer");
        e.clouds.base_resolution = c.at("base_resolution").get<std::uint32_t>();
        require(c.at("detail_resolution").is_number_integer() && c.at("detail_resolution") >= 0 && c.at("detail_resolution") <= 4294967295ULL, "Invalid cloud integer");
        e.clouds.detail_resolution = c.at("detail_resolution").get<std::uint32_t>();
        e.clouds.bottom_height_m = environment_number(c.at("bottom_height_m"));
        e.clouds.top_height_m = environment_number(c.at("top_height_m"));
        e.clouds.coverage = environment_number(c.at("coverage"));
        e.clouds.density = environment_number(c.at("density"));
        e.clouds.extinction_per_m = environment_number(c.at("extinction_per_m"));
        e.clouds.weather_scale_m = environment_number(c.at("weather_scale_m"));
        e.clouds.shape_scale_m = environment_number(c.at("shape_scale_m"));
        e.clouds.detail_scale_m = environment_number(c.at("detail_scale_m"));
        e.clouds.bottom_fade_fraction = environment_number(c.at("bottom_fade_fraction"));
        e.clouds.top_fade_fraction = environment_number(c.at("top_fade_fraction"));
        require(c.at("wind_velocity_mps").is_array() && c.at("wind_velocity_mps").size() == 2, "Cloud wind must have two numbers");
        e.clouds.wind_velocity_mps = {environment_number(c.at("wind_velocity_mps")[0]), environment_number(c.at("wind_velocity_mps")[1])};
    }
    return e;
}
} // namespace dk::detail

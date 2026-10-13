#pragma once
#include <dk/commands/CommandRegistry.hpp>
#include <dk/scene/Environment.hpp>
namespace dk::operations_detail {
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

inline Json environment_schema() {
    const auto vector = schema::array(schema::number(), 3, 3);
    auto uint32 = schema::integer(); uint32["minimum"] = 0; uint32["maximum"] = 4294967295ULL;
    auto version = schema::integer();
    version["enum"] = Json::array({2});
    return schema::object({
        {"version", version},
        {"enabled", schema::boolean()},
        {"clouds", schema::object({{"enabled", schema::boolean()},
            {"seed", uint32},
            {"generator_version", uint32},
            {"weather_resolution", uint32},
            {"base_resolution", uint32},
            {"detail_resolution", uint32},
            {"bottom_height_m", schema::number()},
            {"top_height_m", schema::number()},
            {"coverage", schema::number()},
            {"density", schema::number()},
            {"extinction_per_m", schema::number()},
            {"weather_scale_m", schema::number()},
            {"shape_scale_m", schema::number()},
            {"detail_scale_m", schema::number()},
            {"bottom_fade_fraction", schema::number()},
            {"top_fade_fraction", schema::number()},
            {"wind_velocity_mps", schema::array(schema::number(), 2, 2)}},
            {"enabled", "seed", "generator_version", "weather_resolution", "base_resolution", "detail_resolution", "bottom_height_m", "top_height_m", "coverage", "density", "extinction_per_m", "weather_scale_m", "shape_scale_m", "detail_scale_m", "bottom_fade_fraction", "top_fade_fraction", "wind_velocity_mps"})},
        {"atmosphere", schema::object({{"planet_center_m", vector},
            {"ground_radius_m", schema::number()},
            {"atmosphere_height_m", schema::number()},
            {"rayleigh_scattering_per_m", vector},
            {"rayleigh_scale_height_m", schema::number()},
            {"mie_scattering_per_m", vector},
            {"mie_extinction_per_m", vector},
            {"mie_scale_height_m", schema::number()},
            {"mie_anisotropy", schema::number()},
            {"absorption_extinction_per_m", vector},
            {"absorption_center_height_m", schema::number()},
            {"absorption_half_width_m", schema::number()},
            {"ground_albedo", vector}},
            {"planet_center_m", "ground_radius_m", "atmosphere_height_m", "rayleigh_scattering_per_m", "rayleigh_scale_height_m", "mie_scattering_per_m", "mie_extinction_per_m", "mie_scale_height_m", "mie_anisotropy", "absorption_extinction_per_m", "absorption_center_height_m", "absorption_half_width_m", "ground_albedo"})},
        {"sun", schema::object({{"direction", vector},
            {"irradiance", vector},
            {"angular_radius_rad", schema::number()}},
            {"direction", "irradiance", "angular_radius_rad"})}
    }, {"version", "enabled", "atmosphere", "sun", "clouds"});
}
// Parameters have already passed the registry schema, including nested field/type checks.
inline EnvironmentDescription read_environment(const Json& j) {
    EnvironmentDescription e;
    e.enabled = j.at("enabled").get<bool>();
    e.atmosphere.planet_center_m = j.at("atmosphere").at("planet_center_m").get<EnvironmentVector>();
    e.atmosphere.ground_radius_m = j.at("atmosphere").at("ground_radius_m").get<double>();
    e.atmosphere.atmosphere_height_m = j.at("atmosphere").at("atmosphere_height_m").get<double>();
    e.atmosphere.rayleigh_scattering_per_m = j.at("atmosphere").at("rayleigh_scattering_per_m").get<EnvironmentVector>();
    e.atmosphere.rayleigh_scale_height_m = j.at("atmosphere").at("rayleigh_scale_height_m").get<double>();
    e.atmosphere.mie_scattering_per_m = j.at("atmosphere").at("mie_scattering_per_m").get<EnvironmentVector>();
    e.atmosphere.mie_extinction_per_m = j.at("atmosphere").at("mie_extinction_per_m").get<EnvironmentVector>();
    e.atmosphere.mie_scale_height_m = j.at("atmosphere").at("mie_scale_height_m").get<double>();
    e.atmosphere.mie_anisotropy = j.at("atmosphere").at("mie_anisotropy").get<double>();
    e.atmosphere.absorption_extinction_per_m = j.at("atmosphere").at("absorption_extinction_per_m").get<EnvironmentVector>();
    e.atmosphere.absorption_center_height_m = j.at("atmosphere").at("absorption_center_height_m").get<double>();
    e.atmosphere.absorption_half_width_m = j.at("atmosphere").at("absorption_half_width_m").get<double>();
    e.atmosphere.ground_albedo = j.at("atmosphere").at("ground_albedo").get<EnvironmentVector>();
    e.sun.direction = j.at("sun").at("direction").get<EnvironmentVector>();
    e.sun.irradiance = j.at("sun").at("irradiance").get<EnvironmentVector>();
    e.sun.angular_radius_rad = j.at("sun").at("angular_radius_rad").get<double>();
    e.clouds.enabled = j.at("clouds").at("enabled").get<bool>();
    e.clouds.seed = j.at("clouds").at("seed").get<std::uint32_t>();
    e.clouds.generator_version = j.at("clouds").at("generator_version").get<std::uint32_t>();
    e.clouds.weather_resolution = j.at("clouds").at("weather_resolution").get<std::uint32_t>();
    e.clouds.base_resolution = j.at("clouds").at("base_resolution").get<std::uint32_t>();
    e.clouds.detail_resolution = j.at("clouds").at("detail_resolution").get<std::uint32_t>();
    e.clouds.bottom_height_m = j.at("clouds").at("bottom_height_m").get<double>();
    e.clouds.top_height_m = j.at("clouds").at("top_height_m").get<double>();
    e.clouds.coverage = j.at("clouds").at("coverage").get<double>();
    e.clouds.density = j.at("clouds").at("density").get<double>();
    e.clouds.extinction_per_m = j.at("clouds").at("extinction_per_m").get<double>();
    e.clouds.weather_scale_m = j.at("clouds").at("weather_scale_m").get<double>();
    e.clouds.shape_scale_m = j.at("clouds").at("shape_scale_m").get<double>();
    e.clouds.detail_scale_m = j.at("clouds").at("detail_scale_m").get<double>();
    e.clouds.bottom_fade_fraction = j.at("clouds").at("bottom_fade_fraction").get<double>();
    e.clouds.top_fade_fraction = j.at("clouds").at("top_fade_fraction").get<double>();
    e.clouds.wind_velocity_mps = j.at("clouds").at("wind_velocity_mps").get<std::array<double, 2>>();
    return e;
}
} // namespace dk::operations_detail

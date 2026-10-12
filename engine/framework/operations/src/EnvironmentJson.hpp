#pragma once
#include <dk/commands/CommandRegistry.hpp>
#include <dk/scene/Environment.hpp>
namespace dk::operations_detail {
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

inline Json environment_schema() {
    const auto vector = schema::array(schema::number(), 3, 3);
    auto version = schema::integer();
    version["enum"] = Json::array({1});
    return schema::object({
        {"version", version},
        {"enabled", schema::boolean()},
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
    }, {"version", "enabled", "atmosphere", "sun"});
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
    return e;
}
} // namespace dk::operations_detail

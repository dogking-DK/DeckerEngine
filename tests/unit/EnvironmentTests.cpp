#include "SceneTestFiles.hpp"
#include <dk/scene/SceneIO.hpp>
#include <nlohmann/json.hpp>
#include <cmath>
#include <functional>
#include <limits>
#include <numbers>

using namespace dk;
namespace {
EnvironmentDescription example() {
    EnvironmentDescription e;
    e.enabled = true;
    e.atmosphere.planet_center_m = {2, -6371000, 3};
    e.atmosphere.ground_radius_m = 6371000;
    e.atmosphere.atmosphere_height_m = 90000;
    e.atmosphere.rayleigh_scattering_per_m = {1e-6, 2e-6, 3e-6};
    e.atmosphere.rayleigh_scale_height_m = 8500;
    e.atmosphere.mie_scattering_per_m = {2e-6, 3e-6, 4e-6};
    e.atmosphere.mie_extinction_per_m = {5e-6, 6e-6, 7e-6};
    e.atmosphere.mie_scale_height_m = 1400;
    e.atmosphere.mie_anisotropy = -0.4;
    e.atmosphere.absorption_extinction_per_m = {3e-7, 4e-7, 5e-7};
    e.atmosphere.absorption_center_height_m = 27000;
    e.atmosphere.absorption_half_width_m = 17000;
    e.atmosphere.ground_albedo = {0.1, 0.2, 0.4};
    e.sun.direction = {0.13, -0.7, 0.3};
    e.sun.irradiance = {2, 3, 4};
    e.sun.angular_radius_rad = 0.008;
    e.clouds.enabled=true; e.clouds.seed=4294967295u; e.clouds.weather_resolution=64;
    e.clouds.base_resolution=32; e.clouds.detail_resolution=16;
    e.clouds.bottom_height_m=1200; e.clouds.top_height_m=5000; e.clouds.coverage=.67;
    e.clouds.density=2.4; e.clouds.extinction_per_m=.0004;
    e.clouds.weather_scale_m=83000; e.clouds.shape_scale_m=8400; e.clouds.detail_scale_m=720;
    e.clouds.bottom_fade_fraction=.14; e.clouds.top_fade_fraction=.31; e.clouds.wind_velocity_mps={-7,19};
    return e;
}
}
TEST_CASE("environment Earth scale heights retain submeter precision and signed boundaries", "[environment]") {
    AtmosphereParameters a;
    for (double height : {-100.0, 0.0, 0.001, 1.0, 100000.0, 200000.0}) {
        const auto result = atmosphere_height_m(a, {0, height, 0});
        REQUIRE(result);
        CHECK(std::abs(*result - height) < 1e-6);
    }
    // Independent rationalized tangent-height expression avoids subtracting radii.
    const double x = 10000, r = a.ground_radius_m;
    const double expected = x * x / (std::sqrt(r*r + x*x) + r);
    CHECK(std::abs(*atmosphere_height_m(a, {x, 0, 0}) - expected) < 1e-6);
    a.planet_center_m = {1e8, -r, -1e8};
    CHECK(std::abs(*atmosphere_height_m(a, {1e8, 0.001, -1e8}) - 0.001) < 1e-6);
    CHECK_FALSE(atmosphere_height_m(a, {0, std::numeric_limits<double>::infinity(), 0}));
    CHECK_FALSE(atmosphere_height_m(a, {1e10, 0, 0}));
}
TEST_CASE("environment solar direction is robust and radiance uses normal irradiance", "[environment]") {
    for (const EnvironmentVector direction : {EnvironmentVector{0.13, -0.7, 0.3},
        EnvironmentVector{1e308, 1e308, -1e308}, EnvironmentVector{1e-310, 0, 0}}) {
        auto e = example(); e.sun.direction = direction;
        auto normalized = normalize_environment(e); REQUIRE(normalized);
        const auto& d = normalized->sun.direction;
        CHECK(std::abs(std::hypot(d[0], d[1], d[2]) - 1) < 1e-15);
        for (int i = 0; i < 32; ++i) CHECK(*normalize_environment(*normalized) == *normalized);
    }
    SunParameters sun; sun.angular_radius_rad = 0.1; sun.irradiance = {1, 2, 0};
    const auto radiance = solar_disk_radiance(sun); REQUIRE(radiance);
    // Numerical solid-angle integral of constant disk radiance * cos(theta).
    double integrated = 0; constexpr int steps = 10000;
    const double step = sun.angular_radius_rad / steps;
    for (int i = 0; i < steps; ++i) {
        const double theta = (i + 0.5) * step;
        integrated += (*radiance)[0] * 2 * std::numbers::pi * std::sin(theta) * std::cos(theta) * step;
    }
    CHECK(std::abs(integrated - 1) < 1e-8);
    CHECK((*radiance)[1] == 2 * (*radiance)[0]); CHECK((*radiance)[2] == 0);
    sun.angular_radius_rad = 0; CHECK_FALSE(solar_disk_radiance(sun));
}
TEST_CASE("environment rejects invalid disabled parameters without changing the document", "[environment]") {
    auto doc = SceneDocument::create().value();
    const auto before = doc->snapshot().value();
    const std::vector<std::function<void(EnvironmentDescription&)>> invalid{
        [](auto& e) { e.clouds.generator_version=0; },
        [](auto& e) { e.clouds.weather_resolution=7; },
        [](auto& e) { e.clouds.base_resolution=256; },
        [](auto& e) { e.clouds.detail_resolution=6; },
        [](auto& e) { e.clouds.bottom_height_m=-1; },
        [](auto& e) { e.clouds.bottom_height_m=e.clouds.top_height_m; },
        [](auto& e) { e.clouds.top_height_m=e.atmosphere.atmosphere_height_m; },
        [](auto& e) { e.clouds.coverage=-.1; },
        [](auto& e) { e.clouds.coverage=1.1; },
        [](auto& e) { e.clouds.density=-1; },
        [](auto& e) { e.clouds.extinction_per_m=-.1; },
        [](auto& e) { e.clouds.weather_scale_m=0; },
        [](auto& e) { e.clouds.shape_scale_m=0; },
        [](auto& e) { e.clouds.detail_scale_m=0; },
        [](auto& e) { e.clouds.bottom_fade_fraction=0; },
        [](auto& e) { e.clouds.top_fade_fraction=.6; },
        [](auto& e) { e.clouds.wind_velocity_mps[0]=1001; },
        [](auto& e) { e.clouds.wind_velocity_mps[1]=std::numeric_limits<double>::quiet_NaN(); },
        [](auto& e) { e.atmosphere.planet_center_m[0] = 1e10; },
        [](auto& e) { e.atmosphere.ground_radius_m = 0; },
        [](auto& e) { e.atmosphere.atmosphere_height_m = -1; },
        [](auto& e) { e.atmosphere.rayleigh_scattering_per_m[1] = -1e-8; },
        [](auto& e) { e.atmosphere.rayleigh_scale_height_m = 0; },
        [](auto& e) { e.atmosphere.mie_scattering_per_m[0] = 1; },
        [](auto& e) { e.atmosphere.mie_extinction_per_m[0] = -1; },
        [](auto& e) { e.atmosphere.mie_scale_height_m = 1e6; },
        [](auto& e) { e.atmosphere.mie_anisotropy = 1; },
        [](auto& e) { e.atmosphere.absorption_extinction_per_m[2] = 2; },
        [](auto& e) { e.atmosphere.absorption_center_height_m = -1; },
        [](auto& e) { e.atmosphere.absorption_half_width_m = 0; },
        [](auto& e) { e.atmosphere.ground_albedo[2] = 1.1; },
        [](auto& e) { e.sun.direction = {0, 0, 0}; },
        [](auto& e) { e.sun.direction[1] = std::numeric_limits<double>::infinity(); },
        [](auto& e) { e.sun.irradiance[1] = -1; },
        [](auto& e) { e.sun.angular_radius_rad = 0; },
        [](auto& e) { e.atmosphere.ground_radius_m = std::numeric_limits<double>::quiet_NaN(); }
    };
    for (const auto& mutate : invalid) {
        EnvironmentDescription e; mutate(e);
        REQUIRE_FALSE(doc->set_environment(e));
        CHECK(doc->snapshot()->same_content(before)); CHECK(doc->revision() == 0); CHECK(doc->dirty());
    }
    auto vacuum = example();
    vacuum.atmosphere.rayleigh_scattering_per_m = {};
    vacuum.atmosphere.mie_scattering_per_m = {}; vacuum.atmosphere.mie_extinction_per_m = {};
    vacuum.atmosphere.absorption_extinction_per_m = {}; vacuum.sun.irradiance = {};
    REQUIRE(doc->set_environment(vacuum));
}
TEST_CASE("environment v2 roundtrip retains all values snapshots and source provenance", "[environment]") {
    SceneTestFiles files;
    auto project = Project::create(files.root, {"Environment", "scene.json", {}}).value();
    auto doc = SceneDocument::create().value();
    const auto original = doc->snapshot().value();
    CHECK(original.environment() == EnvironmentDescription{});
    REQUIRE(doc->set_environment(example()));
    const auto snapshot = doc->snapshot().value();
    CHECK_FALSE(snapshot.same_content(original));
    CHECK(snapshot.logical_bytes() >= sizeof(EnvironmentDescription));
    auto text = serialize_scene(snapshot, project).value();
    CHECK(nlohmann::json::parse(text)["version"] == 2);
    for (int i = 0; i < 32; ++i) {
        auto loaded = parse_scene(text, project); REQUIRE(loaded);
        CHECK((*loaded)->environment() == snapshot.environment());
        REQUIRE((*loaded)->set_environment((*loaded)->environment()));
        CHECK((*loaded)->revision() == snapshot.revision());
        CHECK(serialize_scene(*(*loaded)->snapshot(), project).value() == text);
    }
    auto staged = SceneDocument::stage(snapshot).value();
    CHECK(staged->revision() == 0); CHECK(staged->environment() == snapshot.environment());
    REQUIRE(staged->set_environment({}));
    CHECK(doc->apply_snapshot(staged->snapshot().value()).value());
    CHECK(doc->environment() == original.environment()); CHECK(doc->revision() == 2);
    CHECK_FALSE(save_scene(*staged, original, project));
    REQUIRE(save_scene(*doc, snapshot, project)); CHECK(doc->dirty());
    CHECK(load_scene(project).value()->environment() == snapshot.environment());
    REQUIRE(save_scene(*doc, project)); CHECK_FALSE(doc->dirty());
}
TEST_CASE("environment migrates v1 and failed reload preserves the current document", "[environment]") {
    SceneTestFiles files;
    auto project = Project::create(files.root, {"Migration", "scene.json", {}}).value();
    auto doc = SceneDocument::create().value(); REQUIRE(doc->create_entity());
    auto old = nlohmann::json::parse(serialize_scene(*doc->snapshot(), project).value());
    old["version"] = 1; old.erase("environment");
    files.write("scene.json", old.dump());
    auto loaded = load_scene(project).value();
    CHECK(loaded->id() == doc->id()); CHECK(loaded->revision() == doc->revision());
    CHECK_FALSE(loaded->dirty()); CHECK(loaded->environment() == EnvironmentDescription{});
    REQUIRE(save_scene(*loaded, project));
    const auto current = nlohmann::json::parse(serialize_scene(*loaded->snapshot(), project).value());
    CHECK(current["version"] == 2);
    auto atmosphere_only=current; atmosphere_only["environment"]["version"]=1;
    atmosphere_only["environment"].erase("clouds");
    CHECK(parse_scene(atmosphere_only.dump(),project).value()->environment().clouds==CloudParameters{});
    std::vector<nlohmann::json> invalid;
    auto add = [&](auto mutate) { auto j = current; mutate(j); invalid.push_back(std::move(j)); };
    add([](auto& j) { j["version"] = 3; });
    add([](auto& j) { j["version"] = 2.0; });
    add([](auto& j) { j["version"] = 1; }); // v1 cannot smuggle environment.
    add([](auto& j) { j.erase("environment"); });
    add([](auto& j) { j["environment"]["version"] = 3; });
    add([](auto& j) { j["environment"]["clouds"] = {}; });
    add([](auto& j) { j["environment"]["clouds"]["seed"] = -1; });
    add([](auto& j) { j["environment"]["clouds"]["seed"] = 4294967296ULL; });
    add([](auto& j) { j["environment"]["clouds"]["seed"] = 1.0; });
    add([](auto& j) { j["environment"]["clouds"]["wind_velocity_mps"] = {1}; });
    add([](auto& j) { j["environment"]["sun"]["direction"] = {0, 0, 0}; });
    add([](auto& j) { j["environment"]["sun"]["extra"] = 0; });
    add([](auto& j) { j["environment"]["sun"]["irradiance"] = {1, 2}; });
    add([](auto& j) { j["environment"]["atmosphere"]["ground_radius_m"] = "6360000"; });
    const auto before = loaded->snapshot().value(); auto* pointer = loaded.get();
    for (const auto& bad : invalid) {
        files.write("scene.json", bad.dump());
        CHECK_FALSE(reload_scene(loaded, project)); CHECK(loaded.get() == pointer);
        CHECK(loaded->snapshot()->same_content(before)); CHECK_FALSE(loaded->dirty());
    }
    auto maximum = current; maximum["revision"] = std::numeric_limits<std::uint64_t>::max();
    auto exhausted = parse_scene(maximum.dump(), project).value();
    REQUIRE(exhausted->set_environment(exhausted->environment()));
    CHECK_FALSE(exhausted->set_environment(example())); CHECK(exhausted->environment() == EnvironmentDescription{});
}

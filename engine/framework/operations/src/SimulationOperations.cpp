#include <dk/operations/SimulationOperations.hpp>
#include <dk/operations/SceneOperations.hpp>
#include <limits>

namespace dk {
namespace {
Json integer(std::uint64_t minimum, std::uint64_t maximum) {
    auto result = schema::integer(); result["minimum"] = minimum; result["maximum"] = maximum; return result;
}
Json number(double minimum, double maximum) {
    auto result = schema::number(); result["minimum"] = minimum; result["maximum"] = maximum; return result;
}
Json cloth_schema(bool complete = false) {
    const auto scalar = [complete](double low, double high) {
        return complete ? number(static_cast<float>(low), static_cast<float>(high)) : number(low, high);
    };
    const auto properties = Json{{"columns", integer(2, 32)}, {"rows", integer(2, 32)},
        {"seed", integer(0, std::numeric_limits<std::uint32_t>::max())}, {"spacing", scalar(0.01, 1)},
        {"height", scalar(-100, 100)}, {"particle_mass", scalar(0.001, 100)}, {"compliance", scalar(0, 0.01)},
        {"iterations", integer(1, 32)}, {"gravity_y", scalar(-1000, 1000)},
        {"floor_y", scalar(-10000, 10000)}, {"damping", scalar(0, 100)}};
    auto required = Json::array();
    if (complete) for (const auto& [name, value] : properties.items()) { (void)value; required.push_back(name); }
    return schema::object(properties, required);
}
ClothConfig read_cloth(const Json& p) {
    ClothConfig c;
    c.columns = p.value("columns", c.columns); c.rows = p.value("rows", c.rows); c.seed = p.value("seed", c.seed);
    c.spacing = p.value("spacing", c.spacing); c.height = p.value("height", c.height);
    c.particle_mass = p.value("particle_mass", c.particle_mass); c.compliance = p.value("compliance", c.compliance);
    c.physics.iterations = p.value("iterations", c.physics.iterations);
    c.physics.gravity_y = p.value("gravity_y", c.physics.gravity_y); c.physics.floor_y = p.value("floor_y", c.physics.floor_y);
    c.physics.damping = p.value("damping", c.physics.damping);
    return c;
}
Json cloth_json(ClothConfig c) {
    return {{"columns", c.columns}, {"rows", c.rows}, {"seed", c.seed}, {"spacing", c.spacing}, {"height", c.height},
        {"particle_mass", c.particle_mass}, {"compliance", c.compliance}, {"iterations", c.physics.iterations},
        {"gravity_y", c.physics.gravity_y}, {"floor_y", c.physics.floor_y}, {"damping", c.physics.damping}};
}
Json metrics_schema() {
    Json properties{{"particle_count", integer(1, 1024)}, {"constraint_count", integer(0, 8192)}, {"color_count", integer(0, 32)}};
    for (const auto field : {"max_constraint_error", "rms_constraint_error", "max_relative_error", "max_speed",
        "kinetic_energy", "gravity_potential_energy", "compliant_energy", "min_height", "max_penetration", "max_pin_displacement"})
        properties[field] = schema::number();
    auto required = Json::array();
    for (const auto& [name, value] : properties.items()) { (void)value; required.push_back(name); }
    return schema::object(properties, required);
}
Json metrics_json(const XpbdMetrics& m) {
    return {{"particle_count", m.particle_count}, {"constraint_count", m.constraint_count}, {"color_count", m.color_count},
        {"max_constraint_error", m.max_constraint_error}, {"rms_constraint_error", m.rms_constraint_error},
        {"max_relative_error", m.max_relative_error}, {"max_speed", m.max_speed}, {"kinetic_energy", m.kinetic_energy},
        {"gravity_potential_energy", m.gravity_potential_energy}, {"compliant_energy", m.compliant_energy},
        {"min_height", m.min_height}, {"max_penetration", m.max_penetration}, {"max_pin_displacement", m.max_pin_displacement}};
}
Json status_schema() {
    const auto number = integer(0, static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()));
    const auto run = schema::object({{"run_id", schema::string(36, 36)}, {"source", document_state_schema()},
        {"fixed_dt_ns", integer(1000000, 1000000000)}, {"max_catch_up_steps", integer(1, 64)},
        {"steps", number}, {"simulated_time_ns", number}, {"accumulator_ns", number},
        {"dropped_time_ns", number}, {"fault", schema::nullable(schema::string())},
        {"solver", {{"type", "string"}, {"enum", {"none", "xpbd_cpu"}}}},
        {"cloth", schema::nullable(cloth_schema(true))}, {"metrics", schema::nullable(metrics_schema())}},
        {"run_id", "source", "fixed_dt_ns", "max_catch_up_steps", "steps", "simulated_time_ns",
         "accumulator_ns", "dropped_time_ns", "fault", "solver", "cloth", "metrics"});
    return schema::object({{"mode", {{"type", "string"}, {"enum", {"edit", "running", "paused"}}}},
        {"run", schema::nullable(run)}}, {"mode", "run"});
}
Json status_json(const SimulationService& service) {
    const auto state = service.state();
    Json run = nullptr;
    if (state.run) {
        const auto& r = *state.run;
        run = {{"run_id", r.run_id.to_string()}, {"source", document_state_json(r.source)},
            {"fixed_dt_ns", r.config.fixed_dt_ns}, {"max_catch_up_steps", r.config.max_catch_up_steps},
            {"steps", r.clock.steps}, {"simulated_time_ns", r.clock.simulated_time_ns},
            {"accumulator_ns", r.clock.accumulator_ns}, {"dropped_time_ns", r.clock.dropped_time_ns},
            {"fault", r.fault ? Json(error_code_name(*r.fault)) : Json(nullptr)},
            {"solver", r.cloth ? "xpbd_cpu" : "none"}, {"cloth", r.cloth ? cloth_json(*r.cloth) : Json(nullptr)},
            {"metrics", r.metrics ? metrics_json(*r.metrics) : Json(nullptr)}};
    }
    return {{"mode", state.mode == SimulationMode::edit ? "edit" : state.mode == SimulationMode::paused ? "paused" : "running"},
        {"run", std::move(run)}};
}
Result<Json> control_result(const SimulationService& service, Result<void> result) {
    if (!result) return std::unexpected(result.error());
    return status_json(service);
}
}
Result<void> register_simulation_commands(CommandRegistry& registry, SimulationService& service, const SceneService& edit) {
    auto added = registry.add({"simulation.query", "Inspect simulation mode, source and fixed clock", schema::object(),
        status_schema(), CommandEffect::query, false}, [&service](const Json&) -> Result<Json> { return status_json(service); });
    if (!added) return added;
    const auto guard = schema::object({{"document_id", schema::string(36, 36)},
        {"revision", integer(0, std::numeric_limits<std::uint64_t>::max())}}, {"document_id", "revision"});
    added = registry.add({"simulation.start", "Clone the editing scene into an independent simulation world",
        schema::object({{"guard", guard}, {"fixed_dt_ns", integer(1000000, 1000000000)},
            {"max_catch_up_steps", integer(1, 64)}, {"paused", schema::boolean()},
            {"solver", {{"type", "string"}, {"enum", {"none", "xpbd_cpu"}}}}, {"cloth", cloth_schema()}}, {"guard"}),
        status_schema(), CommandEffect::control, false}, [&service, &edit](const Json& p) -> Result<Json> {
            auto g = parse_edit_guard(p["guard"]); if (!g) return std::unexpected(g.error());
            const bool xpbd = p.value("solver", std::string{"none"}) == "xpbd_cpu";
            if (!xpbd && p.contains("cloth"))
                return std::unexpected(Error{ErrorCode::invalid_argument, "cloth requires solver=xpbd_cpu"});
            const auto cloth = xpbd ? std::optional{read_cloth(p.value("cloth", Json::object()))} : std::nullopt;
            return control_result(service, service.start(edit, *g,
                {p.value("fixed_dt_ns", std::int64_t{16666667}), p.value("max_catch_up_steps", std::uint32_t{8})},
                p.value("paused", false), {}, cloth));
        });
    if (!added) return added;
    for (const auto method : {"simulation.pause", "simulation.resume", "simulation.step", "simulation.stop"}) {
        auto parameters = schema::object({{"run_id", schema::string(36, 36)}}, {"run_id"});
        if (std::string_view{method} == "simulation.step") parameters["properties"]["count"] = integer(1, 10000);
        added = registry.add({method, "Control the identified simulation run", parameters, status_schema(), CommandEffect::control, false},
            [&service, method](const Json& p) -> Result<Json> {
                const auto id = SimulationId::parse(p["run_id"].get_ref<const std::string&>());
                if (!id) return std::unexpected(id.error());
                const std::string_view name{method};
                if (name == "simulation.pause") return control_result(service, service.pause(*id));
                if (name == "simulation.resume") return control_result(service, service.resume(*id));
                if (name == "simulation.step") return control_result(service, service.step(*id, p.value("count", std::uint32_t{1})));
                return control_result(service, service.stop(*id));
            });
        if (!added) return added;
    }
    const auto particle = schema::object({{"index", integer(0, 1023)}, {"position", schema::array(schema::number(), 3, 3)},
        {"velocity", schema::array(schema::number(), 3, 3)}, {"inverse_mass", schema::number()}},
        {"index", "position", "velocity", "inverse_mass"});
    return registry.add({"simulation.particles", "Read a versioned page of simulation particles",
        schema::object({{"run_id", schema::string(36, 36)}, {"offset", integer(0, 1024)}, {"limit", integer(1, 256)}}, {"run_id"}),
        schema::object({{"run_id", schema::string(36, 36)}, {"steps", integer(0, std::numeric_limits<std::int64_t>::max())},
            {"simulated_time_ns", integer(0, std::numeric_limits<std::int64_t>::max())}, {"offset", integer(0, 1024)},
            {"total", integer(1, 1024)}, {"has_more", schema::boolean()}, {"particles", schema::array(particle, 0, 256)}},
            {"run_id", "steps", "simulated_time_ns", "offset", "total", "has_more", "particles"}), CommandEffect::query, false},
        [&service](const Json& p) -> Result<Json> {
            auto id = SimulationId::parse(p["run_id"].get_ref<const std::string&>());
            if (!id) return std::unexpected(id.error());
            auto page = service.particle_page(*id, p.value("offset", std::size_t{0}), p.value("limit", std::size_t{128}));
            if (!page) return std::unexpected(page.error());
            auto particles = Json::array();
            for (std::size_t i = 0; i < page->positions.size(); ++i) {
                const auto& position = page->positions[i]; const auto& v = page->velocities[i];
                particles.push_back({{"index", page->offset+i}, {"position", {position.x, position.y, position.z}},
                    {"velocity", {v.x, v.y, v.z}}, {"inverse_mass", position.inverse_mass}});
            }
            return Json{{"run_id", id->to_string()}, {"steps", page->run.clock.steps},
                {"simulated_time_ns", page->run.clock.simulated_time_ns}, {"offset", page->offset}, {"total", page->total},
                {"has_more", page->offset + page->positions.size() < page->total}, {"particles", std::move(particles)}};
        });
}
}

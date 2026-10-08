#include <dk/operations/SimulationOperations.hpp>
#include <dk/operations/SceneOperations.hpp>
#include <limits>

namespace dk {
namespace {
Json integer(std::uint64_t minimum, std::uint64_t maximum) {
    auto result = schema::integer(); result["minimum"] = minimum; result["maximum"] = maximum; return result;
}
Json status_schema() {
    const auto number = integer(0, static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()));
    const auto run = schema::object({{"run_id", schema::string(36, 36)}, {"source", document_state_schema()},
        {"fixed_dt_ns", integer(1000000, 1000000000)}, {"max_catch_up_steps", integer(1, 64)},
        {"steps", number}, {"simulated_time_ns", number}, {"accumulator_ns", number},
        {"dropped_time_ns", number}, {"fault", schema::nullable(schema::string())}},
        {"run_id", "source", "fixed_dt_ns", "max_catch_up_steps", "steps", "simulated_time_ns",
         "accumulator_ns", "dropped_time_ns", "fault"});
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
            {"fault", r.fault ? Json(error_code_name(*r.fault)) : Json(nullptr)}};
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
            {"max_catch_up_steps", integer(1, 64)}, {"paused", schema::boolean()}}, {"guard"}),
        status_schema(), CommandEffect::control, false}, [&service, &edit](const Json& p) -> Result<Json> {
            auto g = parse_edit_guard(p["guard"]); if (!g) return std::unexpected(g.error());
            return control_result(service, service.start(edit, *g,
                {p.value("fixed_dt_ns", std::int64_t{16666667}), p.value("max_catch_up_steps", std::uint32_t{8})},
                p.value("paused", false)));
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
    return {};
}
}

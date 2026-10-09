#include "SceneTestFiles.hpp"
#include <dk/runtime/Runtime.hpp>
#include <dk/operations/SceneOperations.hpp>
#include <limits>

using namespace dk;
using namespace std::chrono_literals;
namespace {
struct Simulation {
    SceneTestFiles files;
    std::unique_ptr<SceneService> edit;
    SimulationService play;
    SimulationService::TimePoint now{};
    Simulation() {
        auto value = SceneService::create(files.root); REQUIRE(value); edit = std::move(*value);
        REQUIRE(edit->new_scene({"Simulation", "scene.json", {}}));
    }
    EditGuard guard() const { const auto s = edit->state(); REQUIRE(s); return {s->document_id, s->revision}; }
    EntityId create() {
        auto id = edit->edit(guard(), CreateEntity{}); REQUIRE(id); REQUIRE(*id); return **id;
    }
    SimulationId start(bool paused = false, FixedStepConfig config = {10000000, 8}) {
        REQUIRE(play.start(*edit, guard(), config, paused, now)); return play.state().run->run_id;
    }
};
struct Commands {
    SceneTestFiles files;
    std::unique_ptr<Runtime> runtime;
    Commands() { auto r = Runtime::create(files.root); REQUIRE(r); runtime = std::move(*r); }
    Result<Json> invoke(std::string_view method, Json p = Json::object(), bool automatic = false) {
        auto r = runtime->dispatch(method, p, automatic); REQUIRE(r); return std::move(r->result);
    }
    Json call(std::string_view method, Json p = Json::object(), bool automatic = false) {
        auto r = invoke(method, std::move(p), automatic); if (!r) FAIL(r.error().message); return *r;
    }
};
}
TEST_CASE("fixed clock preserves fractional time and exact steps across frame partitions") {
    auto a = FixedStepClock::create({10000000, 8}); REQUIRE(a);
    auto b = *a;
    REQUIRE(a->advance(27000000).value() == 2);
    REQUIRE(a->state().accumulator_ns == 7000000);
    for (const auto ns : {3000000, 6000000, 8000000, 10000000}) REQUIRE(b.advance(ns));
    REQUIRE(b.state() == a->state());
    REQUIRE(a->advance(3000000).value() == 1);
    REQUIRE(a->state().simulated_time_ns == 30000000);
    REQUIRE(a->state().accumulator_ns == 0);
    REQUIRE(a->step(10000));
    REQUIRE(a->state().steps == 10003);
    REQUIRE(a->state().simulated_time_ns == 100030000000);
}
TEST_CASE("fixed clock bounds catchup and rejects invalid or overflowing time without mutation") {
    REQUIRE_FALSE(FixedStepClock::create({999999, 8}));
    REQUIRE_FALSE(FixedStepClock::create({1000000001, 8}));
    REQUIRE_FALSE(FixedStepClock::create({1000000, 0}));
    REQUIRE_FALSE(FixedStepClock::create({1000000, 65}));
    auto c = FixedStepClock::create({1000000, 2}); REQUIRE(c);
    REQUIRE(c->advance(17500000).value() == 2);
    REQUIRE(c->state() == FixedStepState{2, 2000000, 500000, 15000000});
    auto before = c->state();
    REQUIRE_FALSE(c->advance(-1)); REQUIRE_FALSE(c->step(0)); REQUIRE_FALSE(c->step(10001));
    REQUIRE_FALSE(c->advance(std::numeric_limits<std::int64_t>::max()));
    REQUIRE(c->state() == before);
    c->discard_fraction();
    REQUIRE(c->advance(std::numeric_limits<std::int64_t>::max() - 15000000));
    before = c->state();
    REQUIRE_FALSE(c->advance(10000000)); // cumulative dropped time is now exhausted
    REQUIRE(c->state() == before);
}
TEST_CASE("simulation pause resume and exact stepping ignore paused wall time") {
    Simulation s;
    const auto id = s.start();
    REQUIRE(s.play.next_deadline(s.now + 1h) == s.now + 10ms);
    s.play.pump(s.now + 25ms);
    REQUIRE(s.play.state().run->clock.steps == 2);
    REQUIRE_FALSE(s.play.step(id));
    REQUIRE(s.play.pause(id)); REQUIRE(s.play.pause(id));
    s.play.pump(s.now + 1h);
    REQUIRE(s.play.state().run->clock.accumulator_ns == 0);
    REQUIRE(s.play.state().run->clock.steps == 2);
    REQUIRE(s.play.next_deadline(s.now + 2h) == s.now + 2h);
    REQUIRE(s.play.step(id, 37));
    REQUIRE(s.play.state().run->clock.steps == 39);
    REQUIRE(s.play.resume(id, s.now + 1h));
    REQUIRE(s.play.resume(id, s.now + 1h + 9ms)); // no-op must not postpone the next tick
    s.play.pump(s.now + 1h + 10ms);
    REQUIRE(s.play.state().run->clock.steps == 40);
    REQUIRE(s.play.state().run->clock.simulated_time_ns == 400000000);
    REQUIRE(s.play.state().run->clock.dropped_time_ns == 0);
}
TEST_CASE("simulation clone survives edit history persistence and document replacement") {
    Simulation s;
    const auto parent = s.create(); const auto child = s.create();
    REQUIRE(s.edit->edit(s.guard(), SetName{child, "play input"}));
    REQUIRE(s.edit->edit(s.guard(), SetParent{child, parent}));
    Trsd trs; trs.translation = Vec3d{1, 2, 3};
    REQUIRE(s.edit->edit(s.guard(), SetTransform{child, trs}));
    REQUIRE(s.edit->save(s.guard())); REQUIRE(s.edit->save_manifest(s.guard(), "project.json"));
    const auto original = s.edit->read_snapshot(s.guard()); REQUIRE(original);
    const auto history = s.edit->history_status();
    const auto id = s.start(true);
    const auto play = s.play.read_snapshot(id); REQUIRE(play);
    REQUIRE(play->scene.same_content(original->scene));
    REQUIRE(play->scene.revision() == 0);
    REQUIRE(play->run.source.document_id == original->state.document_id);
    REQUIRE(play->run.source.revision == original->state.revision);
    REQUIRE(s.edit->history_status().undo_count == history.undo_count);
    REQUIRE(s.edit->edit(s.guard(), SetName{child, "new editing value"}));
    REQUIRE(s.edit->undo(s.guard())); REQUIRE(s.edit->redo(s.guard()));
    REQUIRE(s.edit->save(s.guard()));
    REQUIRE(s.play.step(id, 500));
    REQUIRE(s.play.read_snapshot(id)->scene.same_content(original->scene));
    REQUIRE(s.edit->new_scene({"Replacement", "other.json", {}}, s.guard()));
    REQUIRE(s.edit->load("project.json", s.guard()));
    const auto before_stop = s.edit->read_snapshot(s.guard()); REQUIRE(before_stop);
    const auto stop_history = s.edit->history_status();
    REQUIRE(s.play.stop(id));
    const auto after_stop = s.edit->read_snapshot(s.guard()); REQUIRE(after_stop);
    REQUIRE(document_state_json(after_stop->state) == document_state_json(before_stop->state));
    REQUIRE(after_stop->scene.same_content(before_stop->scene));
    REQUIRE(s.edit->history_status().undo_count == stop_history.undo_count);
    REQUIRE(play->scene.same_content(original->scene)); // owning snapshot survives Stop
    REQUIRE(s.play.state().mode == SimulationMode::edit);
    REQUIRE_FALSE(s.play.read_snapshot(id));
    const auto next = s.start(true); REQUIRE(next != id);
    REQUIRE(s.play.stop(id).error().code == ErrorCode::conflict);
    REQUIRE(s.play.read_snapshot(next)->scene.same_content(after_stop->scene));
}
TEST_CASE("simulation failed starts and controls preserve worlds and expose pump faults") {
    Simulation s;
    s.create();
    auto stale = s.guard(); --stale.revision;
    REQUIRE(s.play.start(*s.edit, stale).error().code == ErrorCode::conflict);
    REQUIRE_FALSE(s.play.start(*s.edit, s.guard(), {0, 8}));
    REQUIRE(s.play.state().mode == SimulationMode::edit);
    const auto id = s.start(true);
    const auto before = s.play.state().run->clock;
    REQUIRE_FALSE(s.play.start(*s.edit, s.guard()));
    REQUIRE_FALSE(s.play.step(id, 0));
    REQUIRE(s.play.pause(SimulationId{}).error().code == ErrorCode::invalid_argument);
    const auto other = SimulationId::generate(); REQUIRE(other);
    REQUIRE(s.play.resume(*other).error().code == ErrorCode::conflict);
    REQUIRE(s.play.step(*other).error().code == ErrorCode::conflict);
    REQUIRE(s.play.state().run->clock == before);
    REQUIRE(s.play.resume(id, s.now + 1s));
    s.play.pump(s.now); // invalid backwards input freezes safely
    REQUIRE(s.play.state().mode == SimulationMode::paused);
    REQUIRE(s.play.state().run->fault == ErrorCode::invalid_argument);
    REQUIRE(s.play.state().run->clock == before);
    REQUIRE_FALSE(s.play.resume(id)); REQUIRE_FALSE(s.play.step(id));
    s.play.shutdown(); REQUIRE_FALSE(s.play.state().run);
    REQUIRE_FALSE(s.play.pause(id));
}
TEST_CASE("simulation commands enforce schemas guards identity and discovery") {
    Commands s;
    REQUIRE(s.call("simulation.query") == Json{{"mode", "edit"}, {"run", nullptr}});
    auto capabilities = s.call("runtime.capabilities")["simulation"];
    REQUIRE(capabilities["fixed_step"] == true); REQUIRE(capabilities["solver"] == "xpbd_cpu");
#ifdef DK_SIMULATION_GPU
    REQUIRE(capabilities["gpu"] == true); REQUIRE(capabilities["experiment_export"] == true);
#else
    REQUIRE(capabilities["gpu"] == false); REQUIRE(capabilities["experiment_export"] == false);
    REQUIRE_FALSE(s.runtime->has_command("simulation.export"));
    REQUIRE_FALSE(s.invoke("simulation.start", {{"solver","xpbd_gpu"}}, true));
#endif
    s.call("scene.new");
    const auto edit = s.call("scene.query");
    const auto guard = Json{{"document_id", edit["state"]["document_id"]}, {"revision", edit["state"]["revision"]}};
    REQUIRE_FALSE(s.invoke("simulation.start", {{"paused", true}}));
    REQUIRE_FALSE(s.invoke("simulation.start", {{"guard", guard}, {"fixed_dt_ns", 1000000.0}}));
    const auto start = s.call("simulation.start", {{"paused", true}}, true); // batch auto-guard
    const auto id = start["run"]["run_id"];
    const auto parsed = SimulationId::parse(id.get<std::string>()); REQUIRE(parsed);
    REQUIRE(s.runtime->read_play_scene(*parsed));
    REQUIRE_FALSE(s.invoke("simulation.step", {{"run_id", id}, {"count", -1}}));
    REQUIRE_FALSE(s.invoke("simulation.step", {{"run_id", id}, {"count", 1.0}}));
    REQUIRE_FALSE(s.invoke("simulation.step", {{"run_id", id}, {"unexpected", 1}}));
    REQUIRE(s.call("simulation.step", {{"run_id", id}, {"count", 77}})["run"]["steps"] == 77);
    REQUIRE(s.call("scene.query") == edit);
    REQUIRE_FALSE(s.invoke("scene.transaction", {{"guard", guard}, {"commands", Json::array({
        {{"method", "simulation.stop"}, {"params", {{"run_id", id}}}}})}}));
    auto methods = s.call("commands.list");
    for (const auto name : {"simulation.query", "simulation.start", "simulation.pause", "simulation.resume", "simulation.step", "simulation.stop"}) {
        bool found = false;
        for (const auto& entry : methods) if (entry["name"] == name) found = true;
        REQUIRE(found);
        const auto d = s.call("commands.describe", {{"name", name}});
        REQUIRE(d["undoable"] == false);
        REQUIRE(d["effect"] == (std::string_view{name} == "simulation.query" ? "query" : "control"));
        REQUIRE(d["result"]["properties"].contains("run"));
    }
    s.call("simulation.resume", {{"run_id", id}});
    s.call("simulation.pause", {{"run_id", id}});
    s.call("simulation.stop", {{"run_id", id}});
    REQUIRE(s.call("scene.query") == edit);
    const auto restarted = s.call("simulation.start", {{"guard", guard}, {"paused", true}});
    REQUIRE(restarted["run"]["run_id"] != id);
    s.call("runtime.shutdown");
    REQUIRE_FALSE(s.runtime->read_play_scene(*SimulationId::parse(restarted["run"]["run_id"].get<std::string>())));
}
TEST_CASE("simulation XPBD commits particles and clock together and isolates the editing world") {
    Simulation s; s.create();
    ClothConfig cloth;
    REQUIRE_FALSE(s.play.start(*s.edit, s.guard(), {1000000000, 8}, true, s.now, cloth));
    REQUIRE(s.play.state().mode == SimulationMode::edit);
    REQUIRE(s.play.start(*s.edit, s.guard(), {10000000, 8}, true, s.now, cloth));
    const auto id = s.play.state().run->run_id;
    const auto initial = s.play.read_particles(id); REQUIRE(initial);
    const auto edit = s.edit->read_snapshot(s.guard()); REQUIRE(edit);
    REQUIRE_FALSE(s.play.step(id, 10000)); // work budget rejection preserves physics and clock
    REQUIRE(s.play.state().run->clock.steps == 0);
    REQUIRE(s.play.read_particles(id)->data.positions == initial->data.positions);
    REQUIRE(s.play.step(id, 10));
    auto after = s.play.read_particles(id); REQUIRE(after);
    REQUIRE(after->run.clock.steps == 10); REQUIRE(after->data.positions != initial->data.positions);
    REQUIRE(after->run.metrics->particle_count == 64);
    s.play.pump(s.now + std::chrono::hours{1});
    REQUIRE(s.play.read_particles(id)->data.positions == after->data.positions);
    REQUIRE(s.play.resume(id, s.now)); s.play.pump(s.now + std::chrono::milliseconds{30});
    REQUIRE(s.play.state().run->clock.steps == 13);
    REQUIRE(s.play.pause(id));
    REQUIRE_FALSE(s.play.particle_page(id, 65)); REQUIRE_FALSE(s.play.particle_page(id, 0, 257));
    const auto page = s.play.particle_page(id, 63, 128); REQUIRE(page);
    REQUIRE(page->positions.size() == 1); REQUIRE(page->total == 64); REQUIRE(page->run.clock.steps == 13);
    REQUIRE(s.play.particle_page(id, 64)->positions.empty());
    REQUIRE(s.play.stop(id));
    REQUIRE(s.edit->read_snapshot(s.guard())->scene.same_content(edit->scene));
    REQUIRE(document_state_json(s.edit->state().value()) == document_state_json(edit->state));
    REQUIRE(initial->data.positions.size() == 64); REQUIRE_FALSE(s.play.read_particles(id));
}
TEST_CASE("simulation XPBD commands validate configuration and return versioned particles and metrics") {
    Commands s; s.call("scene.new");
    REQUIRE_FALSE(s.invoke("simulation.start", {{"cloth", Json::object()}}, true));
    REQUIRE_FALSE(s.invoke("simulation.start", {{"solver", "xpbd_cpu"}, {"cloth", {{"seed", -1}}}}, true));
    REQUIRE_FALSE(s.invoke("simulation.start", {{"solver", "xpbd_cpu"}, {"cloth", {{"rows", 33}}}}, true));
    REQUIRE_FALSE(s.invoke("simulation.start", {{"solver", "xpbd_cpu"}, {"cloth", {{"height", 0}}}}, true));
    // Float32 minimum must validate both input and resolved result schemas.
    auto run = s.call("simulation.start", {{"solver", "xpbd_cpu"}, {"paused", true},
        {"cloth", {{"spacing", 0.01}, {"seed", 4294967295u}}}}, true)["run"];
    REQUIRE(run["solver"] == "xpbd_cpu"); REQUIRE(run["cloth"]["seed"] == 4294967295u);
    const auto id = run["run_id"]; const auto edit = s.call("scene.query");
    const auto original = s.call("simulation.particles", {{"run_id", id}, {"limit", 4}});
    REQUIRE(original["has_more"] == true); REQUIRE(original["total"] == 64);
    REQUIRE(s.runtime->read_play_particles(*SimulationId::parse(id.get<std::string>())));
    run = s.call("simulation.step", {{"run_id", id}, {"count", 10}})["run"];
    REQUIRE(run["metrics"]["max_penetration"] == 0); REQUIRE(run["steps"] == 10);
    const auto page = s.call("simulation.particles", {{"run_id", id}, {"offset", 63}});
    REQUIRE(page["steps"] == 10); REQUIRE(page["particles"].size() == 1);
    REQUIRE(page["has_more"] == false); REQUIRE(page["particles"][0]["index"] == 63);
    const auto d = s.call("commands.describe", {{"name", "simulation.particles"}});
    REQUIRE(d["effect"] == "query"); REQUIRE(d["undoable"] == false);
    REQUIRE(d["parameters"]["required"] == Json::array({"run_id"}));
    REQUIRE_FALSE(s.invoke("simulation.particles", {{"run_id", id}, {"offset", 65}}));
    s.call("simulation.stop", {{"run_id", id}});
    REQUIRE(s.call("scene.query") == edit);
    // A resolved float32 boundary configuration is accepted verbatim for replay.
    const auto replay = s.call("simulation.start", {{"solver","xpbd_cpu"},{"paused",true},{"cloth",run["cloth"]}}, true)["run"];
    REQUIRE(replay["cloth"] == run["cloth"]);
    s.call("simulation.stop", {{"run_id",replay["run_id"]}});
    const auto clock_only = s.call("simulation.start", {{"paused", true}}, true)["run"];
    REQUIRE(clock_only["solver"] == "none"); REQUIRE(clock_only["metrics"].is_null());
    REQUIRE_FALSE(s.invoke("simulation.particles", {{"run_id", clock_only["run_id"]}}));
}

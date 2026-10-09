#include "SceneTestFiles.hpp"
#include <dk/editor/Workspace.hpp>
#include <dk/automation/Client.hpp>
#include <future>
#include <thread>
using namespace dk;
using namespace std::chrono_literals;

TEST_CASE("workspace simulation follows external IPC without replacing next-run drafts") {
    SceneTestFiles files;
    std::filesystem::copy(DK_EDITOR_FIXTURE_DIR,files.root,std::filesystem::copy_options::recursive);
    auto model=editor::Workspace::create(files.root).value(); REQUIRE(model->open("project.json"));
    const auto scene=model->snapshot()->scene, original=scene;
    const auto revision=model->snapshot()->state.revision;
    model->simulation_draft().cloth.seed=123;
    const auto pipe="editor-simulation-"+TaskId::generate()->to_string(); REQUIRE(model->start_ipc(pipe));
    const auto call=[&](std::string method,Json params) {
        auto pending=std::async(std::launch::async,[&] { return ipc::call({pipe},method,params); });
        const auto end=std::chrono::steady_clock::now()+6s;
        while (pending.wait_for(0ms)!=std::future_status::ready && std::chrono::steady_clock::now()<end) {
            model->pump(); std::this_thread::sleep_for(1ms);
        }
        REQUIRE(pending.wait_for(0ms)==std::future_status::ready);
        auto result=pending.get(); REQUIRE(result.exit_code()==0);
    };
    const auto wait=[&](auto predicate) {
        const auto end=std::chrono::steady_clock::now()+5s;
        do { model->pump(); std::this_thread::sleep_for(1ms); }
        while (!predicate(model->simulation_state()) && std::chrono::steady_clock::now()<end);
        REQUIRE(predicate(model->simulation_state()));
    };
    call("simulation.run",{{"guard",{{"document_id",model->snapshot()->state.document_id.to_string()},{"revision",revision}}},
        {"count",20},{"paused",true},{"cloth",{{"seed",42}}}});
    wait([](const auto& s) { return s.mode==SimulationMode::paused; });
    const auto id=model->simulation_state().run->run_id;
    REQUIRE(model->simulation_state().run->cloth->seed==42); REQUIRE(model->simulation_draft().cloth.seed==123);
    REQUIRE(model->control_simulation(id,editor::SimulationControl::step));
    wait([](const auto& s) { return s.mode==SimulationMode::paused; });
    REQUIRE(model->simulation_state().run->clock.steps==1);
    call("simulation.step",{{"run_id",id.to_string()},{"count",2}});
    wait([](const auto& s) { return s.mode==SimulationMode::paused; });
    REQUIRE(model->simulation_state().run->clock.steps==3);
    call("simulation.cancel",{{"run_id",id.to_string()}});
    wait([](const auto& s) { return s.run->task->status==SimulationTaskStatus::cancelled; });
    REQUIRE_FALSE(model->control_simulation(id,editor::SimulationControl::resume));
    call("simulation.stop",{{"run_id",id.to_string()}});
    wait([](const auto& s) { return !s.run; });
    REQUIRE(model->snapshot()->scene.same_content(original)); REQUIRE(model->snapshot()->state.revision==revision);
    REQUIRE(model->history().undo_count==0); REQUIRE(model->simulation_draft().cloth.seed==123);
    model->close_ipc();
}

TEST_CASE("workspace IPC refresh preserves draft guard and follows external manifest")
{
    SceneTestFiles files;
    std::filesystem::copy(DK_EDITOR_FIXTURE_DIR,files.root,std::filesystem::copy_options::recursive);
    auto model = editor::Workspace::create(files.root).value();
    REQUIRE(model->open("project.json"));
    const auto id = model->snapshot()->scene.entities().front().id;
    REQUIRE(model->select(id));
    const auto original = model->draft()->entity.name;
    model->draft()->entity.name = "unsaved local draft"; model->draft()->modified = true;
    const auto draft_revision = model->draft()->guard.revision;
    const auto name = "editor-" + TaskId::generate()->to_string();
    REQUIRE(model->start_ipc(name));
    const auto guard = [&] { return Json{{"document_id",model->snapshot()->state.document_id.to_string()},
        {"revision",model->snapshot()->state.revision}}; };
    const auto call = [&](std::string method, Json params) {
        auto pending = std::async(std::launch::async,[&] { return ipc::call({name},method,params); });
        const auto until = std::chrono::steady_clock::now()+6s;
        while (pending.wait_for(0ms) != std::future_status::ready && std::chrono::steady_clock::now()<until) {
            model->pump(); std::this_thread::sleep_for(1ms);
        }
        REQUIRE(pending.wait_for(0ms) == std::future_status::ready);
        auto response = pending.get(); REQUIRE(response.exit_code() == 0); return response;
    };
    (void)call("entity.set_name",{{"id",id.to_string()},{"name","remote"},{"guard",guard()}});
    REQUIRE(model->pending()); CHECK(model->draft()->entity.name == "unsaved local draft");
    CHECK(model->draft()->guard.revision == draft_revision); CHECK(model->snapshot()->state.revision > draft_revision);
    auto applied = model->apply(); REQUIRE_FALSE(applied); CHECK(applied.error().code == ErrorCode::conflict);
    REQUIRE(model->revert()); CHECK(model->draft()->entity.name == "remote");
    REQUIRE(model->undo()); CHECK(model->draft()->entity.name == original);
    (void)call("project.save",{{"manifest","external.json"},{"guard",guard()}});
    CHECK(model->manifest() == "external.json");
    (void)call("scene.load",{{"manifest","external.json"},{"guard",guard()}});
    CHECK_FALSE(model->selection()); CHECK_FALSE(model->draft()); CHECK(model->history().undo_count == 0);
    (void)call("runtime.shutdown",Json::object()); CHECK(model->stopping()); model->close_ipc();
}

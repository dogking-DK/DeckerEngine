#include "SceneTestFiles.hpp"
#include <dk/editor/Workspace.hpp>
#include <dk/automation/Client.hpp>
#include <future>
#include <thread>
using namespace dk;
using namespace std::chrono_literals;

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

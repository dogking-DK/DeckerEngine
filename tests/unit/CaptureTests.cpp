#include "SceneTestFiles.hpp"
#include <dk/runtime/Runtime.hpp>
#include <catch2/generators/catch_generators.hpp>
using namespace dk;
namespace {
struct CaptureCommands {
    SceneTestFiles files;
    std::unique_ptr<Runtime> runtime=Runtime::create(files.root).value();
    Result<Json> call(std::string_view method,Json p=Json::object(),bool auto_guard=false) {
        auto result=runtime->dispatch(method,p,auto_guard); if (!result) return std::unexpected(result.error());
        return std::move(result->result);
    }
    Json value(std::string_view method,Json p=Json::object(),bool auto_guard=false) {
        auto result=call(method,std::move(p),auto_guard); INFO(method); if (!result) INFO(result.error().message);
        REQUIRE(result); return std::move(*result);
    }
};
}
TEST_CASE("capture discovery and invalid inputs do not start jobs or change scene") {
    CaptureCommands c;
    auto capabilities=c.value("runtime.capabilities"); CHECK(capabilities["render_capture"]==true);
    CHECK(capabilities["capture_limits"]["active"]==3);
    auto descriptor=c.value("commands.describe",{{"name","render.capture"}});
    CHECK(descriptor["effect"]=="external"); CHECK(descriptor["undoable"]==false);
    CHECK_FALSE(c.call("render.capture",{{"output","a.ppm"}}));
    auto state=c.value("scene.new"); auto history=c.value("history.status");
    const Json guard={{"document_id",state["document_id"]},{"revision",state["revision"]}};
    const Json bad=GENERATE(Json{{"width",0}},Json{{"height",2049}},Json{{"unknown",true}},
        Json{{"output","../escape.ppm"}},Json{{"output","a.png"}},Json{{"output","missing/a.ppm"}},
        Json{{"camera",{{"eye",{0,0,0}},{"target",{0,0,0}}}}},
        Json{{"camera",{{"eye",{0,0,0}},{"target",{0,1,0}}}}},
        Json{{"camera",{{"eye",{0,0,0}},{"target",{0,0,1}},{"near",2},{"far",1}}}});
    Json params={{"guard",guard},{"output","capture.ppm"}}; params.update(bad);
    CHECK_FALSE(c.call("render.capture",params));
    CHECK(c.value("scene.query")["state"]==state); CHECK(c.value("history.status")==history);
    CHECK_FALSE(std::filesystem::exists(c.files.root/"capture.ppm"));
    c.value("entity.create",Json::object(),true);
    CHECK(c.call("render.capture",{{"guard",guard},{"output","a.ppm"}}).error().code==ErrorCode::conflict);
    CHECK_FALSE(c.call("scene.transaction",{{"commands",Json::array({{{"method","render.capture"},{"params",{{"output","a.ppm"}}}}})}},true));
}
TEST_CASE("capture import failure reaches Job terminal without GPU or file publication") {
    CaptureCommands c; std::filesystem::copy(DK_CAPTURE_FIXTURE_DIR,c.files.root,std::filesystem::copy_options::recursive);
    std::filesystem::remove(c.files.root/"assets/mask.png"); c.files.write("capture.ppm","previous image");
    const auto state=c.value("scene.load",{{"manifest","project.json"}});
    auto submission=c.runtime->dispatch("render.capture",{{"output","capture.ppm"}},true);
    REQUIRE(submission); REQUIRE(submission->result); const auto ticket=*submission->result;
    CHECK(ticket["revision"]==state["revision"]); CHECK(ticket["document_id"]==state["document_id"]);
    CHECK(ticket["frame"]==1); CHECK(ticket["job_id"]!=submission->task_id.to_string());
    auto deadline=JobQueue::Clock::now()+std::chrono::seconds(5);
    for (;;) {
        auto wait=c.value("jobs.wait",{{"id",ticket["job_id"]},{"timeout_ms",100}});
        if (!wait["timed_out"].get<bool>()) { CHECK(wait["job"]["state"]=="failed"); CHECK(wait["job"]["result"].is_null()); REQUIRE(wait["job"]["error"].is_object()); break; }
        REQUIRE(JobQueue::Clock::now()<deadline);
    }
    CHECK(c.value("jobs.cancel",{{"id",ticket["job_id"]}})["accepted"]==false);
    CHECK(read_file_bytes(c.files.root/"capture.ppm")->size()==14);
    CHECK(c.value("scene.query")["state"]==state);
}
TEST_CASE("capture bounded pending work cancellation and close preserve files") {
    SceneTestFiles files; std::filesystem::copy(DK_CAPTURE_FIXTURE_DIR,files.root,std::filesystem::copy_options::recursive);
    std::filesystem::remove(files.root/"assets/mask.png"); files.write("old.ppm","old");
    auto scene=SceneService::create(files.root).value(); REQUIRE(scene->load("project.json"));
    const auto state=scene->state().value(); const EditGuard guard{state.document_id,state.revision};
    auto service=CaptureService::create("unused-shaders").value(); std::vector<JobId> ids;
    for (int i=0;i<4;++i) {
        auto ticket=service->capture(*scene,guard,{"old.ppm"});
        if (!ticket) { CHECK(ticket.error().code==ErrorCode::invalid_state); break; }
        CHECK(ticket->info.frame.frame==ids.size()+1); ids.push_back(ticket->job);
    }
    REQUIRE(ids.size()>=2); REQUIRE(ids.size()<=3);
    REQUIRE(service->cancel(ids.front())->accepted);
    service->close();
    for (auto id:ids) { CHECK(service->job(id)->state==JobState::cancelled); CHECK_FALSE(service->cancel(id)->accepted); }
    CHECK(read_file_bytes(files.root/"old.ppm")->size()==3);
    CHECK_FALSE(service->capture(*scene,guard,{"old.ppm"}));
}

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
    const Json bad=GENERATE(Json{{"width",0}},Json{{"height",2049}},Json{{"unknown",true}},Json{{"exposure",-1}},Json{{"exposure",1000001}},
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
    CHECK(ticket["provenance"]["environment"]==c.value("environment.query")["environment"]);
    CHECK(ticket["provenance"]["shader_fingerprint"].is_null());
    CHECK(ticket["provenance"]["time_seconds"]==0); CHECK(ticket["provenance"]["exposure"]==1);
    CHECK(ticket["provenance"]["quality"]=="unlit_v1");
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

TEST_CASE("capture sequence budgets modes and schema reject before any publication") {
    CaptureCommands c; const auto state=c.value("scene.new"); const auto history=c.value("history.status");
    const Json camera={{"eye",{0,0,-2}},{"target",{0,0,0}},{"near",1},{"far",10000}};
    const Json sample={{"camera",camera},{"time_seconds",0}};
    const Json base={{"guard",{{"document_id",state["document_id"]},{"revision",state["revision"]}}},
        {"output","old.json"},{"width",16},{"height",16},{"samples",Json::array({sample})}};
    c.files.write("old.json","preserve");
    auto descriptor=c.value("commands.describe",{{"name","render.sequence"}}); CHECK(descriptor["effect"]=="external"); CHECK(descriptor["undoable"]==false);
    const Json bad=GENERATE(Json{{"samples",Json::array()}},Json{{"warmup_frames",1}},Json{{"warmup_frames",65}},Json{{"output","wrong.ppm"}},
        Json{{"width",1024},{"height",1024}},Json{{"render_options",{{"mode","unknown"}}}},
        Json{{"render_options",{{"shadow_resolution",17}}}},Json{{"time_seconds",1}});
    auto params=base; params.update(bad); CHECK_FALSE(c.call("render.sequence",params));
    auto many=base; many["width"]=256; many["height"]=256; many["samples"]=Json::array({sample,sample,sample,sample,sample}); CHECK_FALSE(c.call("render.sequence",many));
    auto invalid=base; invalid["samples"][0]["time_seconds"]=1e10; CHECK_FALSE(c.call("render.sequence",invalid));
    CHECK(c.value("scene.query")["state"]==state); CHECK(c.value("history.status")==history);
    CHECK(read_file_bytes(c.files.root/"old.json")->size()==8);
    CHECK_FALSE(c.call("scene.transaction",{{"commands",Json::array({{{"method","render.sequence"},{"params",base}}})}},true));
}

TEST_CASE("capture pathtrace schema and unsupported combinations preserve state") {
    CaptureCommands c; const auto state=c.value("scene.new"); c.files.write("old.json","previous");
    const Json base={{"guard",{{"document_id",state["document_id"]},{"revision",state["revision"]}}},
        {"renderer","pathtrace"},{"format","linear_json"},{"output","old.json"},{"width",16},{"height",16},
        {"pathtrace",{{"version",1},{"target_spp",3}}}};
    auto descriptor=c.value("commands.describe",{{"name","render.capture"}});
    CHECK(descriptor["parameters"]["properties"]["pathtrace"]["properties"]["target_spp"]["maximum"]==65536);
    const Json bad=GENERATE(Json{{"renderer","raster"}},Json{{"exposure",1}},Json{{"time_seconds",0}},Json{{"render_options",Json::object()}},
        Json{{"warmup_frames",0}},Json{{"profile","strict"}},Json{{"output","old.ppm"}},Json{{"width",1024},{"height",1024}},
        Json{{"pathtrace",{{"version",2},{"target_spp",1}}}},Json{{"pathtrace",{{"version",1},{"target_spp",0}}}},
        Json{{"pathtrace",{{"version",1},{"target_spp",65537}}}},Json{{"pathtrace",{{"version",1},{"target_spp",1},{"max_bounces",2},{"rr_start",3}}}},
        Json{{"pathtrace",{{"version",1},{"target_spp",1},{"environment_rgb",{-1,0,0}}}}});
    auto params=base; params.update(bad); CHECK_FALSE(c.call("render.capture",params));
    params=base; params.erase("pathtrace"); CHECK_FALSE(c.call("render.capture",params));
    CHECK(c.value("scene.query")["state"]==state); CHECK(read_file_bytes(c.files.root/"old.json")->size()==8);
}

TEST_CASE("capture media v2 rejects ambiguous modes and invalid work before publication") {
    CaptureCommands c; const auto state=c.value("scene.new"); const auto history=c.value("history.status");
    c.files.write("old.json","previous");
    const Json base={{"guard",{{"document_id",state["document_id"]},{"revision",state["revision"]}}},
        {"renderer","pathtrace"},{"format","linear_json"},{"output","old.json"},{"width",16},{"height",16}};
    const Json invalid=GENERATE(
        Json{{"version",1},{"target_spp",1},{"medium_mode","scene_media"}},
        Json{{"version",1},{"target_spp",1},{"time_seconds",0}},
        Json{{"version",2},{"target_spp",1}},
        Json{{"version",2},{"target_spp",1},{"medium_mode","scene_media"},{"surface_only",false}},
        Json{{"version",2},{"target_spp",1},{"medium_mode","surface_only"},{"time_seconds",0}},
        Json{{"version",2},{"target_spp",1},{"medium_mode","scene_media"},{"cloud_albedo",1.01}},
        Json{{"version",2},{"target_spp",1},{"medium_mode","scene_media"},{"cloud_anisotropy",-.951}},
        Json{{"version",2},{"target_spp",1},{"medium_mode","scene_media"},{"max_tracking_steps",0}},
        Json{{"version",2},{"target_spp",1},{"medium_mode","scene_media"},{"max_steps_per_path",1048577}},
        Json{{"version",2},{"target_spp",1},{"medium_mode","scene_media"},{"time_seconds",1e10}});
    auto params=base; params["pathtrace"]=invalid; CHECK_FALSE(c.call("render.capture",params));
    CHECK(c.value("scene.query")["state"]==state); CHECK(c.value("history.status")==history);
    CHECK(read_file_bytes(c.files.root/"old.json")->size()==8);
}

TEST_CASE("capture orthographic discovery rejects ambiguous lenses before jobs and preserves state") {
    CaptureCommands c; auto state=c.value("scene.new"); const auto history=c.value("history.status");
    const Json guard={{"document_id",state["document_id"]},{"revision",state["revision"]}};
    for(const char* method:{"render.capture","render.sequence"}) {
        const auto schema=c.value("commands.describe",{{"name",method}});
        auto fields=schema["parameters"]["properties"];
        auto camera=(std::string_view(method)=="render.capture" ? fields["camera"] : fields["samples"]["items"]["properties"]["camera"])["properties"];
        CHECK(camera.contains("projection")); CHECK(camera.contains("ortho_height")); CHECK_FALSE(camera.contains("primary_ray_origin"));
        for(const Json bad:{Json{{"projection","orthographic"}},Json{{"projection","orthographic"},{"ortho_height",0}},
            Json{{"projection","orthographic"},{"ortho_height",-1}},Json{{"projection","orthographic"},{"ortho_height",2},{"fov_y",65}},
            Json{{"ortho_height",2}},Json{{"projection","unknown"},{"ortho_height",2}}}) {
            Json lens={{"eye",{0,0,2}},{"target",{0,0,0}}}; lens.update(bad);
            Json request={{"guard",guard},{"width",16},{"height",16},{"output",std::string_view(method)=="render.capture" ? "old.ppm" : "old.json"}};
            if(std::string_view(method)=="render.capture") request["camera"]=lens;
            else request["samples"]=Json::array({{{"camera",{{"eye",{0,0,2}},{"target",{0,0,0}}}},{"time_seconds",0}},{{"camera",lens},{"time_seconds",1}}});
            auto result=c.call(method,request); REQUIRE_FALSE(result); CHECK(result.error().code==ErrorCode::invalid_argument);
            CHECK(c.value("scene.query")["state"]==state); CHECK(c.value("history.status")==history);
        }
    }
}

TEST_CASE("capture orthographic raster environment accepts jobs and late invalid sequence preserves publication") {
    CaptureCommands c; std::filesystem::copy(DK_CAPTURE_FIXTURE_DIR,c.files.root,std::filesystem::copy_options::recursive);
    std::filesystem::remove(c.files.root/"assets/mask.png"); c.value("scene.load",{{"manifest","project.json"}});
    c.files.write("old.ppm","previous"); c.files.write("old.json","previous");
    auto environment=c.value("environment.query")["environment"]; environment["enabled"]=true;
    c.value("environment.set",{{"environment",environment}},true);
    const auto state=c.value("scene.query")["state"],history=c.value("history.status");
    const Json lens={{"eye",{0,10,2}},{"target",{0,10,0}},{"projection","orthographic"},{"ortho_height",3}};
    auto result=c.call("render.capture",{{"camera",lens},{"output","old.ppm"}},true);
    REQUIRE(result); const auto first_ticket=*result;
    const auto failed=c.value("jobs.wait",{{"id",first_ticket["job_id"]},{"timeout_ms",1000}});
    CHECK_FALSE(failed["timed_out"].get<bool>()); CHECK(failed["job"]["state"]=="failed");
    result=c.call("render.sequence",{{"width",16},{"height",16},{"samples",Json::array({{{"camera",{{"eye",{0,10,2}},{"target",{0,10,0}}}},{"time_seconds",0}},
        {{"camera",lens},{"time_seconds",1}}})},{"output","old.json"}},true);
    REQUIRE(result);
    const auto sequence=c.value("jobs.wait",{{"id",(*result)["job_id"]},{"timeout_ms",1000}});
    CHECK_FALSE(sequence["timed_out"].get<bool>()); CHECK(sequence["job"]["state"]=="failed");
    auto bad=lens; bad["fov_y"]=65;
    result=c.call("render.sequence",{{"width",16},{"height",16},{"samples",Json::array({{{"camera",lens},{"time_seconds",0}},{{"camera",bad},{"time_seconds",1}}})},{"output","old.json"}},true);
    REQUIRE_FALSE(result); CHECK(result.error().code==ErrorCode::invalid_argument);
    CHECK(c.value("scene.query")["state"]==state); CHECK(c.value("history.status")==history);
    CHECK(read_file_bytes(c.files.root/"old.ppm")->size()==8); CHECK(read_file_bytes(c.files.root/"old.json")->size()==8);
}

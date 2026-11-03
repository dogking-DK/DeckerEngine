#include "SceneTestFiles.hpp"
#include "ConsoleAssist.hpp"
#include <dk/editor/Workspace.hpp>
#include <limits>

using namespace dk;
using namespace dk::editor;
namespace {
struct Fixture {
    SceneTestFiles files;
    std::unique_ptr<Workspace> model;
    Fixture(bool supported=true) {
        std::filesystem::copy(DK_EDITOR_FIXTURE_DIR,files.root,std::filesystem::copy_options::recursive);
        model=Workspace::create(files.root).value(); REQUIRE(model->open("project.json"));
        REQUIRE(model->attach_editor_viewport({supported,supported ? "" : "Test GPU has no RT"}));
    }
    Result<Json> call(std::string method,Json params=Json::object()) {
        return model->execute_console(Json{{"method",std::move(method)},{"params",std::move(params)}}.dump());
    }
    Json query() { auto r=call("editor.viewport.query"); INFO((r ? "queried" : r.error().message)); REQUIRE(r); return *r; }
    Json change(Json p) {
        p["guard"]=query()["guard"]; auto r=call("editor.viewport.set",p); INFO((r ? "set" : r.error().message)); REQUIRE(r); return *r;
    }
    Json action(const char* name) { auto r=call(std::string("editor.viewport.")+name,{{"guard",query()["guard"]}}); REQUIRE(r); return *r; }
};
}
TEST_CASE("viewport commands are host only discoverable and complete from shared schema") {
    SceneTestFiles files; auto runner=Runtime::create(files.root).value();
    CHECK_FALSE(runner->has_command("editor.viewport.query"));
    Fixture f; auto catalog=f.model->command_catalog();
    for(const auto* action : {"query","set","pause","resume","reset"}) {
        auto d=f.call("commands.describe",{{"name",std::string("editor.viewport.")+action}}); REQUIRE(d);
        CHECK((*d)["undoable"]==false); CHECK((*d)["effect"]==(std::string_view(action)=="query" ? "query" : "control"));
        CHECK((*d)["help"]["version"]==1);
    }
    std::string text="editor.viewport.set direct_lighting=";
    auto a=dk::editor::detail::assist_console(catalog,{text,text.size(),text.size(),text.size(),1});
    REQUIRE(a.candidates.size()==2); CHECK(a.candidates[1].insert=="\"restir_di\"");
    const auto q=f.query(); CHECK(q["settings"]["target_spp"]==64); CHECK(q["presentation"]["has_image"]==false);
    const auto set_help=f.call("commands.describe",{{"name","editor.viewport.set"}}).value()["help"]["fields"];
    CHECK_FALSE(set_help["/properties/target_spp"].contains("default")); // Omission preserves current values.
    CHECK(set_help["/properties/target_spp"]["examples"][0]==64);
    CHECK_FALSE(f.call("editor.viewport.set",{{"guard",q["guard"]}}));
    CHECK_FALSE(f.call("scene.transaction",{{"guard",{{"document_id",q["guard"]["document_id"]},{"revision",1}}},
        {"commands",Json::array({{{"method","editor.viewport.pause"},{"params",{{"guard",q["guard"]}}}}})}}));
}
TEST_CASE("viewport settings and controls are atomic guarded and preserve scene history files") {
    Fixture f; const auto scene=f.model->snapshot()->state;
    const auto history=f.model->history(); const auto disk=read_file_bytes(f.files.root/"scene.json").value();
    const auto before=f.query();
    auto changed=f.change({{"renderer","pathtrace"},{"medium_mode","scene_media"},{"direct_lighting","restir_di"},
        {"initial_candidates",12},{"temporal_reuse",true},{"spatial_reuse",true},{"target_spp",12},{"seed",UINT32_MAX},
        {"bounces",8},{"environment_rgb",{.2,.3,.4}},{"sky_enabled",false},{"sun",false},{"exposure_ev",2.0},
        {"time_seconds",10},{"cloud_albedo",.8},{"cloud_anisotropy",.5}});
    CHECK(changed["guard"]["revision"]==1); CHECK(changed["settings"]["seed"]==UINT32_MAX);
    CHECK(changed["settings"]["reset_sequence"]==0); CHECK_FALSE(changed["presentation"]["current"].get<bool>());
    CHECK(f.change({{"target_spp",12}})["guard"]==changed["guard"]);
    auto paused=f.action("pause"); CHECK(paused["settings"]["paused"]==true);
    CHECK(f.action("pause")["guard"]==paused["guard"]);
    auto resumed=f.action("resume"); CHECK(resumed["settings"]["paused"]==false);
    auto reset=f.action("reset"); CHECK(reset["settings"]["reset_sequence"]==1);
    CHECK(reset["guard"]["revision"].get<unsigned>()==resumed["guard"]["revision"].get<unsigned>()+1);
    const auto saved=f.query();
    for(auto bad : {Json{{"target_spp",0},{"exposure_ev",1}},Json{{"seed",4294967296ULL}},Json{{"bounces",1.0}},
        Json{{"time_seconds",1e10}},Json{{"initial_candidates",65}},Json{{"environment_rgb",{1,2}}},Json{{"unknown",true}}}) {
        bad["guard"]=saved["guard"]; CHECK_FALSE(f.call("editor.viewport.set",bad)); CHECK(f.query()==saved);
    }
    CHECK_FALSE(f.call("editor.viewport.set",{{"guard",before["guard"]},{"renderer","raster"}})); CHECK(f.query()==saved);
    CHECK(f.model->snapshot()->state.revision==scene.revision); CHECK(f.model->snapshot()->state.dirty==scene.dirty);
    CHECK(f.model->history().undo_count==history.undo_count); CHECK(read_file_bytes(f.files.root/"scene.json").value()==disk);
    auto candidate=f.model->editor_viewport()->settings(); candidate.path.exposure=std::numeric_limits<double>::infinity();
    CHECK_FALSE(f.model->editor_viewport()->set(f.model->editor_viewport()->query()->guard,candidate)); CHECK(f.query()==saved);
}
TEST_CASE("viewport unavailable capability drafts and active gestures reject without state loss") {
    Fixture f(false); const auto before=f.query();
    for(auto p : {Json{{"renderer","pathtrace"}},Json{{"target_spp",64}},Json{{"raster_exposure",2},{"sun",true}}}) {
        p["guard"]=before["guard"]; CHECK_FALSE(f.call("editor.viewport.set",p)); CHECK(f.query()==before);
    }
    CHECK_FALSE(f.call("editor.viewport.pause",{{"guard",before["guard"]}}));
    auto raster=f.change({{"raster_exposure",2},{"unlit_inspection",true}}); CHECK(raster["settings"]["raster_exposure"]==2);
    REQUIRE(f.model->select(f.model->snapshot()->scene.entities().front().id)); f.model->draft()->modified=true;
    CHECK_FALSE(f.query()["blocked_reason"].get<std::string>().empty());
    auto candidate=f.model->editor_viewport()->settings(); candidate.raster_exposure=3;
    CHECK_FALSE(f.model->editor_viewport()->set(f.model->editor_viewport()->query()->guard,candidate));
    REQUIRE(f.model->revert()); f.model->viewport_interacting(true);
    CHECK_FALSE(f.call("editor.viewport.set",{{"guard",raster["guard"]},{"raster_exposure",3}}));
    f.model->viewport_interacting(false); CHECK(f.query()==raster);
}
TEST_CASE("viewport document and host replacement invalidate guards and detach closes callbacks") {
    Fixture f; auto first=f.query(); auto old=f.model->editor_viewport();
    REQUIRE(f.model->open("project.json",true)); auto reloaded=f.query();
    CHECK(first["guard"]["document_id"]!=reloaded["guard"]["document_id"]);
    CHECK_FALSE(f.call("editor.viewport.set",{{"guard",first["guard"]},{"raster_exposure",3}}));
    f.model->detach_editor_viewport(); CHECK_FALSE(old->query()); CHECK_FALSE(f.call("editor.viewport.query"));
    REQUIRE(f.model->attach_editor_viewport({true,{}})); auto replacement=f.query();
    CHECK(replacement["guard"]["view_id"]!=first["guard"]["view_id"]);
    CHECK_FALSE(f.call("editor.viewport.set",{{"guard",reloaded["guard"]},{"raster_exposure",3}}));
    auto active=f.model->editor_viewport(); REQUIRE(f.model->request_shutdown()); CHECK_FALSE(active->query());
    active->present({},{}); CHECK_FALSE(active->query());
}
TEST_CASE("viewport presentation separates accepted state completed source failures and late observations") {
    Fixture f; auto host=f.model->editor_viewport(); auto state=host->query().value();
    EditorViewportPresentation p; p.has_image=true; p.current=true; p.document_id=state.guard.document_id;
    p.camera_revision=host->camera().revision();
    p.scene_revision=f.model->snapshot()->state.revision; p.width=64; p.height=64; p.phase="Complete";
    host->present(state.guard,p); REQUIRE(f.query()["presentation"]["current"]==true);
    const auto shown=f.query()["presentation"]["shown_guard"];
    f.change({{"raster_exposure",2}}); auto wanted=host->query()->guard;
    CHECK(f.query()["presentation"]["current"]==false); CHECK(f.query()["presentation"]["shown_guard"]==shown);
    p.current=false; p.phase="Failed"; p.error="injected backend failure"; host->present(wanted,p);
    const auto failed=f.query(); CHECK(failed["presentation"]["shown_guard"]==shown);
    p.current=true; p.error.clear(); host->present(state.guard,p); CHECK(f.query()==failed);
    host->present(wanted,p); CHECK(f.query()["presentation"]["current"]==true);
}
TEST_CASE("viewport Runtime never fills host guards from scene auto guard") {
    SceneTestFiles files; auto runtime=Runtime::create(files.root).value();
    auto created=runtime->dispatch("scene.new",Json::object()); REQUIRE(created); REQUIRE(created->result);
    auto host=EditorViewportService::create({true,{}},[&]()->Result<EditorViewportContext> {
        auto s=runtime->document_state(); if(!s) return std::unexpected(s.error()); return EditorViewportContext{s->document_id,s->revision,{}};
    }).value();
    REQUIRE(runtime->attach_editor_viewport(host));
    auto rejected=runtime->dispatch("editor.viewport.set",{{"raster_exposure",2}},true); REQUIRE(rejected); CHECK_FALSE(rejected->result);
    CHECK(host->settings().raster_exposure==1);
    runtime.reset(); CHECK_FALSE(host->query());
    host->present({},{}); CHECK_FALSE(host->query());
}

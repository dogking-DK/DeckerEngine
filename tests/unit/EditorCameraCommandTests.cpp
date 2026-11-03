#include "SceneTestFiles.hpp"
#include "ConsoleAssist.hpp"
#include <dk/editor/Workspace.hpp>
#include <dk/editor/CameraNavigation.hpp>
#include <numbers>
using namespace dk;
using namespace dk::editor;
namespace {
struct Fixture {
    SceneTestFiles files;
    std::unique_ptr<Workspace> model;
    Fixture() {
        std::filesystem::copy(DK_EDITOR_FIXTURE_DIR,files.root,std::filesystem::copy_options::recursive);
        model=Workspace::create(files.root).value(); REQUIRE(model->open("project.json"));
        REQUIRE(model->attach_editor_viewport({true,{}},true));
    }
    Result<Json> call(std::string method,Json p=Json::object()) {
        return model->execute_console(Json{{"method",std::move(method)},{"params",std::move(p)}}.dump());
    }
    Json query() { auto r=call("editor.camera.query"); INFO((r ? "queried" : r.error().message)); REQUIRE(r); return *r; }
    Json action(const char* action,Json p=Json::object()) {
        p["guard"]=query()["guard"]; auto r=call(std::string("editor.camera.")+action,p);
        INFO((r ? "accepted" : r.error().message)); REQUIRE(r); return *r;
    }
    void reject(const char* action,Json p) {
        auto before=query(); p["guard"]=before["guard"];
        CHECK_FALSE(call(std::string("editor.camera.")+action,p)); CHECK(query()==before);
    }
};
}
TEST_CASE("camera commands discover only on host and share guard without scene side effects") {
    Fixture f; const auto scene=f.model->snapshot()->state; const auto disk=read_file_bytes(f.files.root/"scene.json").value();
    SceneTestFiles root; auto runner=Runtime::create(root.root).value(); CHECK_FALSE(runner->has_command("editor.camera.query"));
    for(const auto* name : {"query","set","orbit","pan","zoom","frame","reset"}) {
        auto d=f.call("commands.describe",{{"name",std::string("editor.camera.")+name}}); REQUIRE(d);
        CHECK((*d)["effect"]==(std::string_view(name)=="query" ? "query" : "control")); CHECK((*d)["undoable"]==false);
    }
    const auto initial=f.query();
    auto speed=f.action("set",{{"move_speed",9},{"navigation_mode","walk"}});
    CHECK(speed["guard"]!=initial["guard"]); CHECK(speed["render_revision"]==initial["render_revision"]);
    CHECK(speed["guard"]==f.call("editor.viewport.query")->at("guard"));
    CHECK_FALSE(f.call("editor.viewport.set",{{"guard",initial["guard"]},{"raster_exposure",2}}));
    CHECK(f.action("set",{{"move_speed",9}})==speed);
    f.reject("set",{}); f.reject("orbit",{{"yaw_deg",2}});
    f.action("set",{{"navigation_mode","orbit"}});
    const auto before=f.query(); CHECK(f.action("orbit",{{"yaw_deg",0}})==before); CHECK(f.action("pan",{{"up",0}})==before); CHECK(f.action("zoom",{{"steps",0}})==before);
    CHECK(f.model->snapshot()->state.revision==scene.revision); CHECK_FALSE(f.model->dirty()); CHECK(f.model->history().undo_count==0);
    CHECK(read_file_bytes(f.files.root/"scene.json").value()==disk);
    std::string text="editor.camera.set projection=";
    auto assist=dk::editor::detail::assist_console(f.model->command_catalog(),{text,text.size(),text.size(),text.size(),1});
    REQUIRE(assist.candidates.size()==2); CHECK(assist.candidates[0].insert=="\"orthographic\"");
}
TEST_CASE("camera commands units pose orbit pan zoom and frame preserve geometric meaning") {
    Fixture f; f.action("set",{{"eye",{0,0,2}},{"yaw_deg",0},{"pitch_deg",0},{"focus_distance",2}});
    auto orbit=f.action("orbit",{{"yaw_deg",90}}); auto c=f.model->editor_viewport()->camera();
    CHECK(c.pose().eye.isApprox(Vec3d{2,0,0})); CHECK(orbit["render_revision"].get<unsigned>()>1);
    const auto matrix=c.world().matrix(); const auto eye=c.pose().eye;
    f.action("pan",{{"right",2},{"up",3}}); c=f.model->editor_viewport()->camera();
    CHECK(c.pose().eye.isApprox(eye+2*matrix.block<3,1>(0,0)+3*matrix.block<3,1>(0,1)));
    const auto d=c.pose().focus_distance; f.action("zoom",{{"steps",2}}); c=f.model->editor_viewport()->camera();
    CHECK(std::abs(c.pose().focus_distance-d*std::exp(-.3))<1e-12);
    f.action("frame",{{"bounds",{{"minimum",{-1,-1,-1}},{"maximum",{1,1,1}}}}});
    c=f.model->editor_viewport()->camera();
    for(int i=0;i<8;++i) { auto uv=c.project({i&1 ? 1.:-1.,i&2 ? 1.:-1.,i&4 ? 1.:-1.},1); REQUIRE(uv); CHECK((uv->array()>=0).all()); CHECK((uv->array()<=1).all()); }
    f.action("set",{{"projection","orthographic"},{"ortho_height",4},{"standard_view","top"}});
    c=f.model->editor_viewport()->camera(); auto a=c.ray({.1,.2},1).value(),b=c.ray({.9,.8},1).value(); CHECK(a.direction==b.direction);
    const auto old_eye=c.pose().eye; f.action("zoom",{{"steps",1}}); c=f.model->editor_viewport()->camera();
    CHECK(c.pose().eye==old_eye); CHECK(c.settings().lens.ortho_height<4);
}
TEST_CASE("camera commands reject mixed invalid candidates and unsupported projections atomically") {
    Fixture f;
    f.reject("set",{{"move_speed",12},{"fov_y_deg",179}}); f.reject("set",{{"projection","orthographic"},{"navigation_mode","fly"}});
    f.reject("set",{{"standard_view","front"}}); f.reject("set",{{"eye",{0,0,0}},{"standard_view","top"}});
    f.reject("set",{{"automatic_clip",false},{"near_plane",5},{"far_plane",1}});
    f.reject("set",{{"move_speed",.001}}); f.reject("set",{{"eye",{0,1}}}); f.reject("set",{{"unknown",true}});
    f.reject("frame",{}); f.reject("frame",{{"bounds",{{"minimum",{1,1,1}},{"maximum",{-1,-1,-1}}}}});
    f.action("set",{{"navigation_mode","walk"}}); f.action("set",{{"projection","orthographic"}});
    CHECK(f.query()["settings"]["navigation_mode"]=="orbit"); f.action("set",{{"projection","perspective"}});
    CHECK(f.query()["settings"]["navigation_mode"]=="walk");
    f.action("set",{{"projection","orthographic"},{"ortho_height",100000}}); f.reject("set",{{"projection","perspective"}});
}
TEST_CASE("camera commands drafts gestures GUI commits reset and lifetime use one authority") {
    Fixture f; auto host=f.model->editor_viewport(); auto initial=f.query();
    f.model->camera_draft(true); f.reject("set",{{"move_speed",9}});
    auto candidate=host->camera(); auto settings=candidate.settings(); settings.navigation.move_speed=9; REQUIRE(candidate.set_settings(settings,1));
    REQUIRE(host->commit_camera(host->query()->guard,candidate,true)); f.model->camera_draft(false);
    CHECK(f.query()["settings"]["move_speed"]==9); CHECK(f.query()["render_revision"]==initial["render_revision"]);
    f.model->viewport_interacting(true); f.reject("pan",{{"right",1}}); f.model->viewport_interacting(false);
    REQUIRE(f.model->select(f.model->snapshot()->scene.entities().front().id)); f.model->draft()->modified=true;
    candidate.orbit(10,5); REQUIRE(host->commit_camera(host->query()->guard,candidate,true));
    CHECK(f.model->draft()->modified); f.reject("set",{{"move_speed",5}}); REQUIRE(f.model->revert());
    f.model->viewport_interacting(true,true); candidate.orbit(10,5);
    CHECK_FALSE(host->commit_camera(host->query()->guard,candidate,true)); f.model->viewport_interacting(false);
    f.action("orbit",{{"yaw_deg",15},{"pitch_deg",20}}); auto reset=f.action("reset");
    CHECK(reset["settings"]["move_speed"]==9); CHECK(reset["settings"]["eye"]==initial["settings"]["eye"]);
    CHECK(f.action("reset")==reset); CHECK(f.action("reset",{{"defaults",true}})["settings"]==initial["settings"]);
    const auto old=f.query(); REQUIRE(f.model->open("project.json",true)); CHECK_FALSE(f.call("editor.camera.set",{{"guard",old["guard"]},{"move_speed",4}}));
    f.model.reset(); CHECK_FALSE(host->query_camera()); CHECK_FALSE(host->reset_camera({},false));
}
TEST_CASE("camera commands frame resolver verifies document source and is removed on close") {
    Fixture f; auto host=f.model->editor_viewport(); const auto id=f.model->snapshot()->scene.entities().front().id;
    f.reject("frame",{{"entity",id.to_string()}});
    unsigned calls=0; const auto document=f.model->snapshot()->state.document_id;
    host->frame_bounds([&](EntityId e,DocumentId d,std::uint64_t revision)->Result<geometry::Bounds> {
        ++calls; CHECK(e==id); CHECK(d==document); CHECK(revision==1);
        return geometry::Bounds{{-1,-1,-1},{1,1,1}};
    });
    f.action("frame",{{"entity",id.to_string()}}); CHECK(calls==1);
    f.model->camera_draft(true); f.reject("frame",{{"entity",id.to_string()}}); CHECK(calls==1);
    f.model->detach_editor_viewport(); CHECK_FALSE(host->frame_camera({},id)); CHECK(calls==1);
}
TEST_CASE("camera commands displayed provenance goes stale on pose but speed preserves render source") {
    Fixture f; auto host=f.model->editor_viewport(); auto state=host->query().value();
    EditorViewportPresentation p; p.has_image=true; p.current=true; p.document_id=state.guard.document_id;
    p.scene_revision=1; p.camera_revision=host->camera().revision(); host->present(state.guard,p);
    REQUIRE(f.query()["presentation"]["current"]==true);
    auto speed=f.action("set",{{"move_speed",4}}); CHECK(speed["render_revision"]==p.camera_revision);
    host->present(host->query()->guard,p); CHECK(f.query()["presentation"]["current"]==true);
    f.action("orbit",{{"yaw_deg",10}}); p.current=true; host->present(host->query()->guard,p);
    CHECK(f.query()["presentation"]["current"]==false); // Reject a current claim for an old camera.
    CHECK(f.query()["presentation"]["shown_guard"]==speed["guard"]);
}
TEST_CASE("camera commands derive current environment clipping without waiting for GUI and keep render revision monotonic") {
    Fixture f; auto original=f.query();
    auto environment=f.call("environment.query").value(); environment["environment"]["enabled"]=true;
    REQUIRE(f.call("environment.set",{{"guard",{{"document_id",environment["state"]["document_id"]},{"revision",environment["state"]["revision"]}}},{"environment",environment["environment"]}}));
    auto lit=f.query(); CHECK(lit["effective_clip"]["near_plane"]==1); CHECK(lit["render_revision"]>original["render_revision"]);
    f.reject("set",{{"automatic_clip",false},{"near_plane",.01},{"far_plane",10000}});
    REQUIRE(f.call("editor.viewport.set",{{"guard",lit["guard"]},{"renderer","pathtrace"}}));
    auto path=f.query(); CHECK(path["effective_clip"]["near_plane"]==.05); CHECK(path["render_revision"]>lit["render_revision"]);
    REQUIRE(f.call("editor.viewport.set",{{"guard",path["guard"]},{"renderer","raster"}}));
    CHECK(f.query()["render_revision"]>path["render_revision"]); CHECK(f.query()["guard"]==f.query()["guard"]);
}

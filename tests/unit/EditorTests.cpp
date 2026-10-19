#include "SceneTestFiles.hpp"
#include <dk/editor/Workspace.hpp>
#include <dk/editor/Camera.hpp>
#include <dk/editor/CameraNavigation.hpp>
#include <dk/scene/SceneDocument.hpp>
#include <catch2/catch_approx.hpp>
#include <algorithm>
#include <thread>
#include <limits>
using namespace dk;
using namespace dk::editor;
namespace {
struct EditorFixture {
    SceneTestFiles files;
    std::unique_ptr<Workspace> model;
    EditorFixture() {
        std::filesystem::copy(DK_EDITOR_FIXTURE_DIR,files.root,std::filesystem::copy_options::recursive);
        model=Workspace::create(files.root).value();
        REQUIRE(model->open("project.json"));
    }
    EntityId select() {
        const auto id=model->snapshot()->scene.entities().front().id;
        REQUIRE(model->select(id)); return id;
    }
};
}
TEST_CASE("workspace selection and drafts do not mutate scene until one guarded transaction") {
    EditorFixture f; auto& model=*f.model;
    const auto id=f.select(),other=model.snapshot()->scene.entities()[1].id;
    const auto revision=model.snapshot()->state.revision;
    const auto original=model.draft()->entity;
    model.draft()->entity.name="Renamed"; model.draft()->entity.local.translation.x()+=0.25;
    model.draft()->modified=true;
    CHECK(model.snapshot()->state.revision==revision); CHECK(model.history().undo_count==0);
    CHECK_FALSE(model.select(other)); CHECK(model.selection()==id);
    CHECK_FALSE(model.undo()); CHECK_FALSE(model.open("project.json"));
    REQUIRE(model.apply());
    CHECK(model.snapshot()->state.revision==revision+1);
    CHECK(model.history().undo_count==1); CHECK_FALSE(model.pending()); CHECK(model.dirty());
    REQUIRE(model.undo()); CHECK(model.draft()->entity.name==original.name);
    CHECK(model.draft()->entity.local.translation==original.local.translation);
    REQUIRE(model.redo()); CHECK(model.draft()->entity.name=="Renamed");
    CHECK(model.draft()->entity.local.translation.x()==original.local.translation.x()+0.25);
    CHECK_FALSE(model.select(EntityId::generate().value())); CHECK(model.selection()==id);
}
TEST_CASE("workspace failed edit and stale draft preserve document history and user input") {
    EditorFixture f; auto& model=*f.model; f.select();
    const auto before=model.snapshot()->scene;
    model.draft()->entity.name="Keep my draft";
    model.draft()->entity.local.rotation.coeffs().setZero();
    model.draft()->modified=true;
    CHECK_FALSE(model.apply()); CHECK(model.pending()); CHECK(model.draft()->entity.name=="Keep my draft");
    CHECK(model.snapshot()->scene.same_content(before)); CHECK(model.history().undo_count==0);
    REQUIRE(model.revert()); CHECK_FALSE(model.pending());
    model.draft()->guard.revision+=1; model.draft()->modified=true;
    CHECK(model.apply().error().code==ErrorCode::conflict);
    CHECK(model.pending()); CHECK(model.snapshot()->scene.same_content(before));
    REQUIRE(model.refresh()); CHECK(model.pending()); // refresh never silently discards a user's draft.
    REQUIRE(model.revert());
    CHECK(model.draft()->guard.revision==model.snapshot()->state.revision);
}
TEST_CASE("workspace save reload and failed open preserve the appropriate session") {
    EditorFixture f; auto& model=*f.model; const auto id=f.select();
    const auto session=model.snapshot()->state.document_id;
    model.draft()->entity.name="Persisted"; model.draft()->modified=true;
    REQUIRE(model.save()); CHECK_FALSE(model.dirty());
    REQUIRE(model.open("project.json"));
    CHECK(model.snapshot()->state.document_id!=session); CHECK_FALSE(model.selection()); CHECK_FALSE(model.draft());
    CHECK(model.history().undo_count==0); REQUIRE(model.select(id)); CHECK(model.draft()->entity.name=="Persisted");
    const auto current=model.snapshot()->state.document_id;
    model.draft()->entity.name="Do not discard"; model.draft()->modified=true;
    CHECK_FALSE(model.open("missing.json",true));
    CHECK(model.snapshot()->state.document_id==current); CHECK(model.selection()==id);
    CHECK(model.draft()->entity.name=="Do not discard"); CHECK(model.pending());
    REQUIRE(model.open("project.json",true)); CHECK_FALSE(model.pending()); CHECK_FALSE(model.selection());
}
TEST_CASE("workspace failed save retains dirty document and undo history") {
    EditorFixture f; auto& model=*f.model; f.select();
    model.draft()->entity.name="Unsaved"; model.draft()->modified=true; REQUIRE(model.apply());
    const auto scene=model.snapshot()->project.scene_path().value();
    const auto backup=scene.parent_path()/"save-original.json";
    std::filesystem::rename(scene,backup); std::filesystem::create_directory(scene);
    const auto session=model.snapshot()->state.document_id; const auto selection=model.selection().value();
    CHECK_FALSE(model.save());
    CHECK(model.snapshot()->state.document_id==session); CHECK(model.selection()==selection);
    CHECK(model.snapshot()->state.dirty); CHECK(model.history().undo_count==1);
    std::filesystem::remove(scene); std::filesystem::rename(backup,scene);
    REQUIRE(model.save()); CHECK_FALSE(model.dirty());
    REQUIRE(model.undo()); CHECK(model.dirty());
}
TEST_CASE("runtime read scene returns independent guarded snapshot") {
    EditorFixture f;
    auto runtime=Runtime::create(f.files.root).value();
    auto loaded=runtime->dispatch("scene.load",{{"manifest","project.json"}}).value().result.value();
    const EditGuard guard{DocumentId::parse(loaded["document_id"].get<std::string>()).value(),loaded["revision"].get<std::uint64_t>()};
    auto old=runtime->read_scene(guard); REQUIRE(old);
    const auto count=old->scene.entities().size();
    auto result=runtime->dispatch("entity.create",{{"guard",{{"document_id",guard.document_id.to_string()},{"revision",guard.revision}}}});
    REQUIRE(result); REQUIRE(result->result);
    CHECK(runtime->read_scene(guard).error().code==ErrorCode::conflict);
    CHECK(old->scene.entities().size()==count); CHECK(old->state.revision==guard.revision);
}
TEST_CASE("workspace simulation draft and controls preserve Edit and reject invalid or stale requests") {
    using namespace std::chrono_literals;
    EditorFixture f; auto& model=*f.model; f.select();
    const auto original=model.snapshot()->scene;
    const auto revision=model.snapshot()->state.revision;
    const auto dirty=model.snapshot()->state.dirty;
    const auto wait=[&](auto predicate) {
        const auto end=std::chrono::steady_clock::now()+5s;
        do { model.pump(); std::this_thread::sleep_for(1ms); }
        while (!predicate(model.simulation_state()) && std::chrono::steady_clock::now()<end);
        REQUIRE(predicate(model.simulation_state()));
    };
    model.draft()->modified=true; REQUIRE_FALSE(model.start_simulation(true)); REQUIRE(model.pending());
    REQUIRE(model.revert());
    auto& draft=model.simulation_draft(); draft.target_steps=3; draft.cloth.rows=0;
    REQUIRE_FALSE(model.start_simulation(true)); REQUIRE(draft.cloth.rows==0); REQUIRE_FALSE(model.simulation_state().run);
    draft.cloth.rows=8; draft.fixed_dt_ns=0;
    REQUIRE_FALSE(model.start_simulation(true)); REQUIRE_FALSE(model.simulation_state().run);
    draft.fixed_dt_ns=10000000;
    REQUIRE(model.start_simulation(true));
    wait([](const auto& state) { return state.mode==SimulationMode::paused; });
    const auto id=model.simulation_state().run->run_id;
    REQUIRE(model.simulation_state().run->clock.steps==0);
    REQUIRE_FALSE(model.simulation_state().run->metrics_sample);
    REQUIRE(model.sample_simulation(id,0));
    REQUIRE(model.simulation_state().run->metrics_sample->steps==0);
    draft.cloth.rows=16; REQUIRE(model.simulation_state().run->cloth->rows==8);
    REQUIRE_FALSE(model.start_simulation());
    REQUIRE(model.control_simulation(SimulationId::generate().value(),SimulationControl::step).error().code==ErrorCode::conflict);
    REQUIRE(model.control_simulation(id,SimulationControl::step));
    wait([](const auto& state) { return state.mode==SimulationMode::paused; });
    REQUIRE(model.simulation_state().run->clock.steps==1);
    REQUIRE(model.simulation_state().run->metrics_sample->steps==0);
    REQUIRE(model.sample_simulation(id,0).error().code==ErrorCode::conflict);
    REQUIRE(model.sample_simulation(SimulationId::generate().value(),1).error().code==ErrorCode::conflict);
    REQUIRE(model.simulation_state().run->metrics_sample->steps==0);
    REQUIRE(model.sample_simulation(id,1));
    REQUIRE(model.simulation_state().run->metrics_sample->simulated_time_ns==10000000);
    REQUIRE(model.control_simulation(id,SimulationControl::resume));
    wait([](const auto& state) { return state.run->task->status==SimulationTaskStatus::succeeded; });
    REQUIRE(model.simulation_state().run->clock.steps==3);
    REQUIRE_FALSE(model.control_simulation(id,SimulationControl::step));
    REQUIRE(model.control_simulation(id,SimulationControl::stop));
    wait([](const auto& state) { return !state.run; });
    REQUIRE(model.start_simulation(true));
    wait([](const auto& state) { return state.mode==SimulationMode::paused; });
    REQUIRE(model.simulation_state().run->cloth->rows==16);
    REQUIRE_FALSE(model.simulation_state().run->metrics_sample);
    REQUIRE(model.control_simulation(id,SimulationControl::stop).error().code==ErrorCode::conflict);
    REQUIRE(model.snapshot()->scene.same_content(original)); REQUIRE(model.snapshot()->state.revision==revision);
    REQUIRE(model.snapshot()->state.dirty==dirty); REQUIRE(model.history().undo_count==0);
}
TEST_CASE("camera projected positions match rays across resize orbit pan and focus") {
    Camera camera{true};
    for (double aspect:{0.5,1.0,2.3}) {
        for (const Vec2d uv:{Vec2d{0.2,0.3},Vec2d{0.5,0.5},Vec2d{0.8,0.6}}) {
            auto ray=camera.ray(uv,aspect).value();
            auto projected=camera.project(ray.origin+ray.direction*4,aspect); REQUIRE(projected);
            CHECK(projected->isApprox(uv,1e-10)); CHECK(ray.direction.norm()==Catch::Approx(1));
        }
        camera.orbit(80,40); camera.pan(15,-10,600); camera.dolly(2);
    }
    camera.orbit(0,100000); camera.dolly(100000); CHECK(camera.world().matrix().allFinite());
    camera.dolly(-100000); REQUIRE(camera.frame({{-1,-2,-3},{1,2,3}},0.5));
    CHECK(camera.project({0,0,0},0.5)->isApprox(Vec2d{0.5,0.5},1e-10));
    CHECK_FALSE(camera.ray({-0.1,0.5},1)); CHECK_FALSE(camera.ray({0.5,0.5},0));
    const auto before=camera.revision(); camera.orbit(0,0); camera.dolly(0); CHECK(camera.revision()==before);
}
TEST_CASE("interaction preview cancel no-op commit and many updates create one undo unit") {
    EditorFixture f; auto& model=*f.model; const auto id=f.select();
    const auto before=model.snapshot()->scene; const auto revision=model.snapshot()->state.revision;
    { auto edit=model.begin_transform().value(); REQUIRE(edit.translate({1,2,3})); }
    CHECK(model.snapshot()->scene.same_content(before)); CHECK(model.history().undo_count==0); CHECK_FALSE(model.dirty());
    auto edit=model.begin_transform().value(); REQUIRE(model.commit_transform(edit));
    CHECK(model.snapshot()->state.revision==revision); CHECK(model.history().undo_count==0);
    for (int i=1;i<=25;++i) REQUIRE(edit.translate({i*0.02,0,0}));
    CHECK(model.snapshot()->scene.same_content(before));
    REQUIRE(model.commit_transform(edit)); CHECK(model.history().undo_count==1); CHECK(model.snapshot()->state.revision==revision+1);
    const auto moved=model.draft()->entity.local.translation;
    REQUIRE(model.undo()); CHECK(model.snapshot()->scene.same_content(before));
    REQUIRE(model.redo()); CHECK(model.draft()->entity.local.translation.isApprox(moved));
    REQUIRE(model.save()); REQUIRE(model.open("project.json")); REQUIRE(model.select(id));
    CHECK(model.draft()->entity.local.translation.isApprox(moved));
}
TEST_CASE("interaction stale selection pending draft and invalid candidate preserve state") {
    EditorFixture f; auto& model=*f.model; f.select();
    auto stale=model.begin_transform().value(); REQUIRE(stale.translate({0.3,0,0}));
    auto edit=stale; REQUIRE(model.commit_transform(edit));
    const auto content=model.snapshot()->scene; const auto history=model.history().undo_count;
    CHECK(model.commit_transform(stale).error().code==ErrorCode::conflict);
    edit=model.begin_transform().value(); edit.value.rotation.coeffs().setZero();
    CHECK_FALSE(model.commit_transform(edit)); CHECK(model.snapshot()->scene.same_content(content));
    CHECK(model.history().undo_count==history);
    edit=model.begin_transform().value(); REQUIRE(edit.translate({0.1,0,0})); REQUIRE(model.select(std::nullopt));
    CHECK(model.commit_transform(edit).error().code==ErrorCode::conflict);
    REQUIRE(model.select(edit.entity)); model.draft()->modified=true;
    CHECK_FALSE(model.begin_transform()); CHECK_FALSE(model.commit_transform(edit));
}
TEST_CASE("interaction local TRS respects rotated scaled parent without decomposing world shear") {
    auto scene=SceneDocument::create().value(); const auto parent=scene->create_entity().value(),child=scene->create_entity().value();
    Trsd p; p.scale={2,3,-1}; p.rotation=Quatd{Eigen::AngleAxisd{0.7,Vec3d::UnitY()}};
    Trsd c; c.translation={1,2,3}; c.rotation=Quatd{Eigen::AngleAxisd{0.4,Vec3d::UnitZ()}};
    REQUIRE(scene->set_local_transform(parent,p)); REQUIRE(scene->set_local_transform(child,c)); REQUIRE(scene->set_parent(child,parent));
    auto snapshot=scene->snapshot().value(); auto parent_world=entity_world(snapshot,parent).value();
    CHECK(entity_world(snapshot,child)->matrix().isApprox(scene->world_transform(child)->matrix()));
    TransformEdit edit{{DocumentId{},0},child,c,c,parent_world}; const auto world=edit.world().value();
    const Vec3d delta{0.2,-0.7,1.1}; REQUIRE(edit.translate(delta));
    CHECK(Vec3d{edit.world()->matrix().block<3,1>(0,3)-world.matrix().block<3,1>(0,3)}.isApprox(delta));
    REQUIRE(edit.rotate(2,0.5)); CHECK(edit.value.rotation.angularDistance(c.rotation*Quatd{Eigen::AngleAxisd{0.5,Vec3d::UnitZ()}})<1e-10);
    REQUIRE(edit.scale(1,1.5)); CHECK(edit.value.scale.y()==1.5); CHECK(edit.value.translation==c.translation);
    CHECK_FALSE(edit.rotate(3,1)); CHECK_FALSE(edit.scale(0,0));
    p.scale.y()=0; edit.parent=Transformd::from_trs(p).value(); CHECK_FALSE(edit.translate(delta));
}

TEST_CASE("workspace environment draft uses guarded commands history and persistence") {
    EditorFixture f; auto& model=*f.model;
    REQUIRE(model.environment_draft());
    const auto original=model.environment_draft()->value;
    const auto revision=model.snapshot()->state.revision;
    model.environment_draft()->modified=true;
    REQUIRE(model.apply_environment()); // no-op does not create history
    CHECK(model.history().undo_count==0); CHECK(model.snapshot()->state.revision==revision);
    model.environment_draft()->value["enabled"]=true; model.environment_draft()->modified=true;
    CHECK(model.pending()); CHECK_FALSE(model.snapshot()->scene.environment().enabled);
    REQUIRE(model.apply_environment());
    CHECK(model.snapshot()->scene.environment().enabled); CHECK(model.history().undo_count==1);
    REQUIRE(model.undo()); CHECK(model.environment_draft()->value==original);
    REQUIRE(model.redo()); CHECK(model.environment_draft()->value["enabled"]==true);
    model.environment_draft()->value["clouds"]["seed"]=42;
    model.environment_draft()->value["sun"]["irradiance"]={2,3,4}; model.environment_draft()->modified=true;
    REQUIRE(model.save()); CHECK_FALSE(model.dirty());
    const auto saved=model.environment_draft()->value;
    REQUIRE(model.open("project.json")); CHECK(model.environment_draft()->value==saved);
    CHECK(model.history().undo_count==0);
}
TEST_CASE("workspace environment invalid and stale drafts retain input and reject partial save") {
    EditorFixture f; auto& model=*f.model; f.select();
    const auto original=model.environment_draft()->value;
    model.environment_draft()->value["sun"]["direction"]={0,0,0}; model.environment_draft()->modified=true;
    REQUIRE_FALSE(model.apply_environment()); CHECK(model.pending()); CHECK(model.history().undo_count==0);
    REQUIRE(model.refresh()); CHECK(model.environment_draft()->value["sun"]["direction"]==Json::array({0,0,0}));
    REQUIRE(model.revert_environment()); CHECK(model.environment_draft()->value==original);
    model.environment_draft()->guard.revision+=1; model.environment_draft()->modified=true;
    REQUIRE(model.apply_environment().error().code==ErrorCode::conflict); CHECK(model.pending());
    REQUIRE(model.revert_environment());
    model.environment_draft()->value["enabled"]=true; model.environment_draft()->modified=true;
    model.draft()->entity.name="Conflicting local drafts"; model.draft()->modified=true;
    REQUIRE_FALSE(model.save()); CHECK(model.history().undo_count==0); CHECK(model.pending());
    CHECK_FALSE(model.snapshot()->scene.environment().enabled);
    REQUIRE(model.revert()); REQUIRE(model.save()); CHECK(model.snapshot()->scene.environment().enabled);
}
TEST_CASE("environment camera uses compatible projection and matching pick rays") {
    Camera camera{true}; const auto revision=camera.revision();
    camera.set_environment(true); CHECK(camera.revision()==revision+1); CHECK(camera.near_plane()==1);
    const auto ray=camera.ray({.3,.6},1.6).value();
    CHECK(camera.project(ray.origin+ray.direction*4,1.6)->isApprox(Vec2d{.3,.6},1e-10));
    camera.set_environment(false); CHECK(camera.near_plane()==.05);
}

TEST_CASE("camera lens settings keep pose and align rays framing pan and gizmo scale") {
    for(double fov : {10.,45.,95.,120.}) for(double aspect : {.5,1.,2.5}) {
        Camera camera{true}; camera.orbit(50,30);
        const auto before=camera.world().matrix(); auto settings=camera.settings();
        settings.lens.fov_y=fov; REQUIRE(camera.set_settings(settings,aspect));
        CHECK(camera.world().matrix()==before);
        const geometry::Bounds bounds{{-2,-1,-3},{4,2,5}};
        REQUIRE(camera.frame(bounds,aspect));
        for(int i=0;i<8;++i) {
            const Vec3d p{i&1 ? 4. : -2.,i&2 ? 2. : -1.,i&4 ? 5. : -3.};
            auto uv=camera.project(p,aspect); REQUIRE(uv);
            CHECK((uv->array()>=0).all()); CHECK((uv->array()<=1).all());
        }
        for(Vec2d uv : {Vec2d{.1,.8},Vec2d{.5,.5},Vec2d{.9,.2}}) {
            auto ray=camera.ray(uv,aspect).value();
            CHECK(camera.project(ray.origin+ray.direction*20,aspect)->isApprox(uv,1e-10));
        }
        const Vec3d center{1,.5,1}; const auto m=camera.world().matrix();
        const double units=camera.units_per_pixel(center,600);
        const auto a=camera.project(center,aspect).value();
        const auto b=camera.project(center+m.block<3,1>(0,0)*units*80,aspect).value();
        CHECK((b.x()-a.x())*600*aspect==Catch::Approx(80));
        camera.pan(12,-7,600);
        const auto c=camera.project(center,aspect).value();
        CHECK((c.x()-a.x())*600*aspect==Catch::Approx(12));
        CHECK((c.y()-a.y())*600==Catch::Approx(-7));
    }
}
TEST_CASE("camera settings commit atomically and navigation preferences preserve render revision") {
    Camera camera{true}; auto settings=camera.settings(); const auto initial=camera.revision();
    settings.navigation.move_speed=20; settings.navigation.orbit_sensitivity=.012;
    settings.navigation.fast_multiplier=8; settings.navigation.invert_y=true;
    REQUIRE(camera.set_settings(settings,1)); CHECK(camera.revision()==initial);
    REQUIRE(camera.set_settings(settings,1)); CHECK(camera.revision()==initial);
    settings.lens.fov_y=45; REQUIRE(camera.set_settings(settings,1)); CHECK(camera.revision()==initial+1);
    const auto pose=camera.world().matrix(); const auto revision=camera.revision();
    for(double bad : {1.,179.,std::numeric_limits<double>::quiet_NaN(),std::numeric_limits<double>::infinity()}) {
        auto candidate=settings; candidate.lens.fov_y=bad;
        CHECK_FALSE(camera.set_settings(candidate,1)); CHECK(camera.settings()==settings);
        CHECK(camera.revision()==revision); CHECK(camera.world().matrix()==pose);
    }
    auto bad=settings; bad.navigation.move_speed=0; bad.lens.fov_y=90;
    CHECK_FALSE(camera.set_settings(bad,1)); CHECK(camera.settings()==settings);
    bad=settings; bad.lens.automatic_clip=false; bad.lens.near_plane=100; bad.lens.far_plane=1;
    CHECK_FALSE(camera.set_settings(bad,1)); CHECK(camera.revision()==revision);
    camera.orbit(12,10); camera.reset(true); CHECK(camera.settings()==settings);
    CHECK(camera.world().matrix()==pose);
    REQUIRE(camera.restore_defaults(true,1)); CHECK(camera.settings()==CameraSettings{});
    const auto restored=camera.revision(); REQUIRE(camera.restore_defaults(true,1)); CHECK(camera.revision()==restored);
    camera.dolly(100000); const auto close=camera.revision(); camera.dolly(100000); CHECK(camera.revision()==close);
}
TEST_CASE("camera environment policy rejects unsupported lenses and frame failure preserves state") {
    Camera camera{true}; camera.set_environment(true); REQUIRE(camera.validate(1));
    auto settings=camera.settings(); settings.lens.fov_y=120;
    REQUIRE(camera.set_settings(settings,1)); const auto revision=camera.revision();
    CHECK_FALSE(camera.validate(10)); CHECK(camera.revision()==revision);
    auto bad=settings; bad.lens.automatic_clip=false;
    CHECK_FALSE(camera.set_settings(bad,1)); CHECK(camera.settings()==settings);
    bad.lens.near_plane=2; REQUIRE(camera.set_settings(bad,1)); CHECK(camera.near_plane()==2);
    const auto manual=camera.revision(); camera.set_environment(false);
    CHECK(camera.near_plane()==2); CHECK(camera.revision()==manual);
    bad.lens.near_plane=.05; REQUIRE(camera.set_settings(bad,1));
    camera.set_environment(true); CHECK_FALSE(camera.validate(1));
    REQUIRE(camera.restore_defaults(true,1)); CHECK(camera.near_plane()==1);
    const auto before=camera.world().matrix(); const auto stable=camera.revision();
    CHECK_FALSE(camera.frame({{-1e6,-1e6,-1e6},{1e6,1e6,1e6}},1));
    CHECK(camera.world().matrix()==before); CHECK(camera.revision()==stable);
    CHECK_FALSE(camera.frame({},1)); CHECK(camera.revision()==stable);
    REQUIRE(camera.frame({{0,0,0},{0,0,0}},1)); REQUIRE(camera.project({0,0,0},1));
}

TEST_CASE("workspace cloud preset is an unapplied draft with undo redo and reload parity") {
    EditorFixture f; auto& m=*f.model; const auto original=m.environment_draft()->value;
    const auto state=m.snapshot()->state;
    REQUIRE(m.preset_environment("sparse")); CHECK(m.pending()); CHECK(m.snapshot()->state.document_id==state.document_id); CHECK(m.snapshot()->state.revision==state.revision); CHECK(m.snapshot()->state.dirty==state.dirty);
    const auto preset=m.environment_draft()->value;
    CHECK(preset["clouds"]["seed"]==1729); REQUIRE_FALSE(m.preset_environment("bad")); CHECK(m.environment_draft()->value==preset);
    REQUIRE(m.apply_environment()); CHECK(m.history().undo_count==1); REQUIRE(m.undo()); CHECK(m.environment_draft()->value==original);
    REQUIRE(m.redo()); CHECK(m.environment_draft()->value==preset); REQUIRE(m.save()); REQUIRE(m.open("project.json")); CHECK(m.environment_draft()->value==preset);
    REQUIRE(m.preset_environment("overcast")); m.environment_draft()->guard.revision+=1;
    CHECK(m.apply_environment().error().code==ErrorCode::conflict); CHECK(m.pending());
    REQUIRE(m.revert_environment()); CHECK(m.environment_draft()->value==preset);
}

TEST_CASE("workspace console shares explicit guards and protects drafts without blocking waits") {
    EditorFixture f; auto& model=*f.model; const auto before=model.snapshot()->state;
    auto query=model.execute_console(R"({"method":"scene.query","params":{}})"); REQUIRE(query);
    CHECK((*query)["state"]["revision"]==before.revision);
    CHECK_FALSE(model.execute_console("invalid"));
    CHECK_FALSE(model.execute_console(std::string(8193,' ')));
    CHECK_FALSE(model.execute_console(R"({"method":"jobs.wait","params":{"timeout_ms":1000}})"));
    CHECK_FALSE(model.execute_console(R"({"method":"entity.create","params":{}})"));
    const Json params={{"guard",{{"document_id",before.document_id.to_string()},{"revision",before.revision}}}};
    const auto command=Json{{"method","entity.create"},{"params",params}}.dump();
    f.select(); model.draft()->modified=true;
    CHECK_FALSE(model.execute_console(command)); REQUIRE(model.execute_console(R"({"method":"scene.query","params":{}})"));
    REQUIRE(model.revert()); REQUIRE(model.execute_console(command));
    CHECK(model.snapshot()->state.revision==before.revision+1); CHECK(model.history().undo_count==1);
    CHECK_FALSE(model.execute_console(command)); // stale explicit guard
    REQUIRE(model.undo()); CHECK(model.snapshot()->scene.entities().size()==query->at("entities").size());
}

TEST_CASE("camera navigation is frame rate independent normalized and uses speed modifiers") {
    for(auto mode : {NavigationMode::fly,NavigationMode::walk}) for(int hz : {30,60,144}) {
        for(auto axes : {Vec3d{0,0,1},Vec3d{1,0,1},Vec3d{1,1,1}}) for(int modifier : {0,1,2,3}) {
            Camera camera{true}; REQUIRE(camera.set_navigation_mode(mode));
            CameraNavigation nav; nav.begin(); NavigationInput input;
            input.move=axes; input.seconds=1./hz; input.fast=modifier&1; input.slow=modifier&2;
            const Vec3d start=camera.world().matrix().block<3,1>(0,3);
            REQUIRE(nav.update(camera,input)); CHECK(camera.world().matrix().block<3,1>(0,3)==start);
            for(int i=0;i<hz;++i) REQUIRE(nav.update(camera,input));
            const Vec3d delta=camera.world().matrix().block<3,1>(0,3)-start;
            CHECK(delta.norm()==Catch::Approx(3*(input.slow ? .25 : input.fast ? 4 : 1)).margin(1e-10));
            if(mode==NavigationMode::walk) CHECK(delta.y()==0);
        }
    }
}
TEST_CASE("camera navigation preserves mode transition pose and walk height through look and frame") {
    Camera camera{true}; camera.orbit(43,250);
    for(auto mode : {NavigationMode::fly,NavigationMode::walk,NavigationMode::orbit}) {
        const auto pose=camera.world().matrix(); const auto revision=camera.revision();
        REQUIRE(camera.set_navigation_mode(mode)); CHECK(camera.world().matrix()==pose); CHECK(camera.revision()==revision);
    }
    REQUIRE(camera.set_navigation_mode(NavigationMode::walk)); CameraNavigation nav; nav.begin(); REQUIRE(nav.update(camera,{}));
    const double height=camera.world().matrix()(1,3);
    NavigationInput input; input.seconds=.05; input.move={1,1,1}; input.look={150,1e8};
    for(int i=0;i<10;++i) { REQUIRE(nav.update(camera,input)); CHECK(camera.world().matrix()(1,3)==height); }
    const auto held=camera.world().matrix(); input.move={0,1,0}; input.look.setZero(); REQUIRE(nav.update(camera,input)); CHECK(camera.world().matrix()==held);
    REQUIRE(camera.frame({{-1,4,-1},{1,6,1}},1)); const double framed=camera.world().matrix()(1,3);
    input.move={0,0,1}; REQUIRE(nav.update(camera,input)); CHECK(camera.world().matrix()(1,3)==framed);
    camera.reset(true); REQUIRE(nav.update(camera,input)); CHECK(camera.world().matrix()(1,3)==0);
    REQUIRE(camera.restore_defaults(true,1)); CHECK(camera.navigation_mode()==NavigationMode::orbit);
}
TEST_CASE("camera navigation clamps stalls cancels and rejects invalid input atomically") {
    Camera camera{true}; REQUIRE(camera.set_navigation_mode(NavigationMode::fly)); CameraNavigation nav;
    NavigationInput input; input.move={0,0,1}; input.seconds=100;
    const auto initial=camera.world().matrix(); REQUIRE(nav.update(camera,input)); CHECK(camera.world().matrix()==initial);
    nav.begin(); REQUIRE(nav.update(camera,input)); REQUIRE(nav.update(camera,input));
    CHECK(Vec3d{camera.world().matrix().block<3,1>(0,3)-initial.block<3,1>(0,3)}.norm()==Catch::Approx(.15));
    const auto revision=camera.revision(); const auto pose=camera.world().matrix(); const auto settings=camera.settings();
    for(int field=0;field<4;++field) {
        auto bad=input; bad.wheel=2;
        if(field==0) bad.seconds=-1;
        if(field==1) bad.move.x()=2;
        if(field==2) bad.look.x()=std::numeric_limits<double>::infinity();
        if(field==3) bad.wheel=std::numeric_limits<double>::quiet_NaN();
        CHECK_FALSE(nav.update(camera,bad)); CHECK(camera.world().matrix()==pose); CHECK(camera.revision()==revision); CHECK(camera.settings()==settings);
    }
    REQUIRE(nav.update(camera,{})); CHECK(camera.revision()==revision);
    nav.cancel(); REQUIRE(nav.update(camera,input)); CHECK(camera.world().matrix()==pose);
    nav.begin(); REQUIRE(nav.update(camera,input)); CHECK(camera.world().matrix()==pose);
    CHECK_FALSE(camera.set_navigation_mode(static_cast<NavigationMode>(99))); CHECK(camera.navigation_mode()==NavigationMode::fly);
}
TEST_CASE("camera navigation mouse ignores dt and wheel changes only preference") {
    Camera a{true},b{true}; REQUIRE(a.set_navigation_mode(NavigationMode::fly)); REQUIRE(b.set_navigation_mode(NavigationMode::fly));
    CameraNavigation x,y; x.begin(); y.begin(); REQUIRE(x.update(a,{})); REQUIRE(y.update(b,{}));
    NavigationInput input; input.look={30,15}; input.seconds=1./144; REQUIRE(x.update(a,input));
    input.seconds=1./30; REQUIRE(y.update(b,input)); CHECK(a.world().matrix().isApprox(b.world().matrix()));
    const auto revision=a.revision(); input={}; input.wheel=1; REQUIRE(x.update(a,input));
    CHECK(a.settings().navigation.move_speed==Catch::Approx(3*std::exp(.15))); CHECK(a.revision()==revision);
    input.wheel=1e8; REQUIRE(x.update(a,input)); CHECK(a.settings().navigation.move_speed==1000); REQUIRE(x.update(a,input)); CHECK(a.revision()==revision);
    auto settings=b.settings(); settings.navigation.invert_y=true; REQUIRE(b.set_settings(settings,1)); b.reset(true);
    input={}; input.look={30,15}; REQUIRE(y.update(b,input));
    CHECK(b.world().matrix()(1,2)==Catch::Approx(-a.world().matrix()(1,2)));
}

TEST_CASE("camera navigation follows viewing axes independently of lens") {
    for(double fov : {30.,100.}) for(int axis=0;axis<3;++axis) {
        Camera camera{true}; camera.orbit(80,40); auto settings=camera.settings(); settings.lens.fov_y=fov;
        REQUIRE(camera.set_settings(settings,1)); REQUIRE(camera.set_navigation_mode(NavigationMode::fly));
        const auto before=camera.world().matrix(); CameraNavigation nav; nav.begin(); REQUIRE(nav.update(camera,{}));
        NavigationInput input; input.move[axis]=1; input.seconds=.04;
        REQUIRE(nav.update(camera,input));
        const Vec3d expected=axis==0 ? Vec3d{before.block<3,1>(0,0)} : axis==1 ? Vec3d{0,1,0} : Vec3d{-before.block<3,1>(0,2)};
        const Vec3d delta=camera.world().matrix().block<3,1>(0,3)-before.block<3,1>(0,3);
        CHECK(delta.isApprox(expected*.12,1e-10));
        REQUIRE(camera.set_navigation_mode(NavigationMode::walk)); const Vec3d start=camera.world().matrix().block<3,1>(0,3);
        input.move={0,0,1}; REQUIRE(nav.update(camera,input));
        const Vec3d horizontal=Vec3d{-before(0,2),0,-before(2,2)}.normalized();
        CHECK(Vec3d{camera.world().matrix().block<3,1>(0,3)-start}.isApprox(horizontal*.12,1e-10));
    }
}

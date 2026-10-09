#include "SceneTestFiles.hpp"
#include <dk/editor/Workspace.hpp>
#include <dk/editor/Camera.hpp>
#include <dk/scene/SceneDocument.hpp>
#include <catch2/catch_approx.hpp>
#include <algorithm>
#include <thread>
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
    draft.cloth.rows=16; REQUIRE(model.simulation_state().run->cloth->rows==8);
    REQUIRE_FALSE(model.start_simulation());
    REQUIRE(model.control_simulation(SimulationId::generate().value(),SimulationControl::step).error().code==ErrorCode::conflict);
    REQUIRE(model.control_simulation(id,SimulationControl::step));
    wait([](const auto& state) { return state.mode==SimulationMode::paused; });
    REQUIRE(model.simulation_state().run->clock.steps==1);
    REQUIRE(model.control_simulation(id,SimulationControl::resume));
    wait([](const auto& state) { return state.run->task->status==SimulationTaskStatus::succeeded; });
    REQUIRE(model.simulation_state().run->clock.steps==3);
    REQUIRE_FALSE(model.control_simulation(id,SimulationControl::step));
    REQUIRE(model.control_simulation(id,SimulationControl::stop));
    wait([](const auto& state) { return !state.run; });
    REQUIRE(model.start_simulation(true));
    wait([](const auto& state) { return state.mode==SimulationMode::paused; });
    REQUIRE(model.simulation_state().run->cloth->rows==16);
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
    camera.dolly(-100000); camera.frame({{-1,-2,-3},{1,2,3}},0.5);
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

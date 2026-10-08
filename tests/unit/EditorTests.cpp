#include "SceneTestFiles.hpp"
#include <dk/editor/Workspace.hpp>
#include <algorithm>
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
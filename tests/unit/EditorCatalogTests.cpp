#include "SceneTestFiles.hpp"
#include <dk/editor/Workspace.hpp>
#include <algorithm>

using namespace dk;
TEST_CASE("catalog workspace reads preserve drafts selection history and task ledger") {
    SceneTestFiles files;
    std::filesystem::copy(DK_EDITOR_FIXTURE_DIR,files.root,std::filesystem::copy_options::recursive);
    auto model=editor::Workspace::create(files.root).value();
    const auto version=model->command_catalog_version();
    REQUIRE_FALSE(model->command_catalog().commands.empty()); // no document required
    REQUIRE(model->open("project.json"));
    REQUIRE(model->select(model->snapshot()->scene.entities().front().id));
    model->draft()->entity.name="保留草稿"; model->draft()->modified=true;
    model->environment_draft()->value["enabled"]=true; model->environment_draft()->modified=true;
    const auto entity=model->selection();
    const auto scene=model->snapshot()->state;
    const auto environment=model->environment_draft()->value;
    const auto draft_guard=model->draft()->guard;
    const auto history=model->history();
    const auto disk=read_file_bytes(files.root/"scene.json").value();
    const auto first=model->execute_console("tasks.list").value();
    const auto control=model->execute_console("tasks.list").value();
    const auto* published=model->snapshot();
    auto catalog=model->command_catalog();
    for (int i=0;i<100;++i) {
        REQUIRE(model->command_catalog_version()==version);
        REQUIRE(model->command_catalog().commands.size()==catalog.commands.size());
    }
    CHECK(model->snapshot()==published); CHECK(model->selection()==entity); CHECK(model->pending());
    CHECK(model->snapshot()->state.revision==scene.revision); CHECK(model->snapshot()->state.document_id==scene.document_id);
    CHECK(model->snapshot()->state.dirty==scene.dirty);
    CHECK(model->draft()->entity.name=="保留草稿"); CHECK(model->draft()->guard.document_id==draft_guard.document_id);
    CHECK(model->draft()->guard.revision==draft_guard.revision);
    CHECK(model->environment_draft()->value==environment); CHECK(model->environment_draft()->modified);
    CHECK(model->history().undo_count==history.undo_count); CHECK(model->history().redo_count==history.redo_count);
    CHECK(read_file_bytes(files.root/"scene.json").value()==disk);
    const auto after=model->execute_console("tasks.list").value();
    REQUIRE(after.size()-control.size()==control.size()-first.size());
    for (std::size_t i=0;i<control.size()-first.size();++i) CHECK(after[control.size()+i]["method"]==control[first.size()+i]["method"]);
    const auto query=std::ranges::find(catalog.commands,std::string("scene.query"),&CommandDescriptor::name);
    REQUIRE(query!=catalog.commands.end()); REQUIRE(query->help);
    CHECK(query->help->at("fields")["/properties/limit"]["default"]==128);
    REQUIRE(model->open("project.json",true));
    CHECK(model->snapshot()->state.document_id!=scene.document_id); CHECK(model->command_catalog_version()==version);
    model.reset(); CHECK(query->help->at("version")==1);
}

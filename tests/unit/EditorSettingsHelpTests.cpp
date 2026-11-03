#include "SceneTestFiles.hpp"
#include "SettingsCatalog.hpp"
#include "CommandFieldHelp.hpp"
#include <dk/editor/Workspace.hpp>
#include <dk/editor/Console.hpp>
#include <algorithm>
#include <set>
using namespace dk;
using namespace dk::editor;
using namespace dk::editor::detail;
namespace {
struct Fixture {
    SceneTestFiles files;
    std::unique_ptr<Workspace> model;
    Fixture() {
        std::filesystem::copy(DK_EDITOR_FIXTURE_DIR,files.root,std::filesystem::copy_options::recursive);
        model=Workspace::create(files.root).value(); REQUIRE(model->open("project.json"));
        REQUIRE(model->attach_editor_viewport({true,{}},true));
    }
};
const SettingInfo& get(const std::vector<SettingInfo>& entries,std::string_view id) {
    auto it=std::ranges::find(entries,id,&SettingInfo::id); REQUIRE(it!=entries.end()); return *it;
}
}
TEST_CASE("settings help catalog binds actual schemas and shares Console help without invented defaults") {
    Fixture f; const auto catalog=f.model->command_catalog(); const auto entries=build_settings_catalog(catalog);
    CHECK(entries.size()>100); std::set<std::string> ids;
    for(const auto& s:entries) {
        CHECK(ids.insert(s.id).second); CHECK_FALSE(s.explanation.empty()); CHECK_FALSE(s.impact.empty());
        if(s.command.empty()) { CHECK(s.command_template.empty()); continue; }
        INFO(s.id); REQUIRE(s.command_available); CHECK_FALSE(s.help.empty()); CHECK_FALSE(s.command_template.empty());
    }
    const auto& lens=get(entries,"camera.fov_y_deg");
    auto command=std::ranges::find(catalog.commands,std::string("editor.camera.set"),&CommandDescriptor::name);
    REQUIRE(command!=catalog.commands.end());
    CHECK(lens.help==command_field_help(&*command,lens.path,command->parameters.at(Json::json_pointer(lens.path)),false));
    CHECK(lens.help.find("unit: deg")!=std::string::npos); CHECK(lens.help.find("default:")==std::string::npos);
    CHECK(get(entries,"simulation.fixed_dt_ns").help.find("default: 10000000")!=std::string::npos);
    CHECK(get(entries,"inspector.translation").help.find("unit: m")!=std::string::npos);
    CHECK(get(entries,"path.environment_rgb").help.find("items.maximum: 1000")!=std::string::npos);
}
TEST_CASE("settings help search matches Chinese aliases canonical names multiple tokens and bounded input") {
    Fixture f; const auto entries=build_settings_catalog(f.model->command_catalog());
    auto found=search_settings(entries,"相机 视场角"); REQUIRE(found.indices.size()==1); CHECK(entries[found.indices[0]].id=="camera.fov_y_deg");
    found=search_settings(entries,"PATH TARGET_SPP"); REQUIRE(found.indices.size()==1); CHECK(entries[found.indices[0]].id=="path.target_spp");
    found=search_settings(entries,"云 覆盖率"); REQUIRE(found.indices.size()==1); CHECK(entries[found.indices[0]].id=="environment.clouds.coverage");
    found=search_settings(entries,""); CHECK(found.indices.size()==64); CHECK(found.truncated);
    CHECK(search_settings(entries,std::string(257,'x')).error.size()>0);
    CHECK(search_settings(entries,"two\nlines").error.size()>0); CHECK(search_settings(entries,std::string("\xff",1)).error.size()>0);
    CHECK(search_settings(entries,"missing-unrelated-parameter").indices.empty());
}
TEST_CASE("settings help templates preserve all required objects and explicit identity placeholders") {
    Fixture f; const auto catalog=f.model->command_catalog(); const auto entries=build_settings_catalog(catalog);
    for(const auto& s:entries) if(!s.command_template.empty()) {
        INFO(s.id); auto parsed=parse_console_command(s.command_template); REQUIRE(parsed);
        CHECK(parsed->method==s.command);
        const auto d=std::ranges::find(catalog.commands,s.command,&CommandDescriptor::name); REQUIRE(d!=catalog.commands.end());
        for(const auto& key:d->parameters.value("required",Json::array())) CHECK(parsed->params.contains(key.get<std::string>()));
        CHECK(s.command_template.find(f.model->snapshot()->state.document_id.to_string())==std::string::npos);
    }
    auto camera=parse_console_command(get(entries,"camera.move_speed").command_template).value();
    CHECK(camera.params["guard"]["view_id"]=="<view_id>"); CHECK(camera.params["guard"]["revision"]=="<revision>");
    auto env=parse_console_command(get(entries,"environment.sun.irradiance").command_template).value();
    REQUIRE(env.params["environment"].contains("clouds")); REQUIRE(env.params["environment"].contains("atmosphere"));
    CHECK(env.params["environment"]["sun"]["irradiance"].size()==3);
    auto trs=parse_console_command(get(entries,"inspector.translation").command_template).value();
    CHECK(trs.params["transform"].contains("rotation")); CHECK(trs.params["transform"].contains("scale"));
    CHECK(get(entries,"preview.time_seconds").command_template.empty());
}
TEST_CASE("settings help unavailable reasons distinguish mode draft capability and independent views") {
    Fixture f; const auto entries=build_settings_catalog(f.model->command_catalog()); SettingsContext c;
    CHECK(setting_unavailable(get(entries,"inspector.translation"),c).find("Select an entity")!=std::string::npos);
    c.scene=true; c.path_available=true;
    CHECK(setting_unavailable(get(entries,"path.temporal_reuse"),c).find("ReSTIR")!=std::string::npos);
    CHECK(setting_unavailable(get(entries,"camera.near_plane"),c).find("Automatic")!=std::string::npos);
    c.orthographic=true; CHECK(setting_unavailable(get(entries,"camera.fov_y_deg"),c).find("Perspective")!=std::string::npos);
    c.simulation_view=true; CHECK(setting_unavailable(get(entries,"camera.move_speed"),c).find("independent")!=std::string::npos);
    CHECK(setting_unavailable(get(entries,"viewport.renderer"),c).find("Edit view")!=std::string::npos);
    c.camera_popup=true; CHECK(setting_unavailable(get(entries,"path.target_spp"),c).find("Camera popup")!=std::string::npos);
    c.path_available=false; c.path_reason="No ray-tracing device"; CHECK(setting_unavailable(get(entries,"path.target_spp"),c)==c.path_reason);
    CHECK(setting_unavailable(get(entries,"simulation.pause"),c).find("Start or Prepare")!=std::string::npos);
    CHECK(setting_unavailable(get(entries,"simulation.gravity_y"),c).empty()); // next-run draft remains editable
}
TEST_CASE("settings help absent commands never invent templates and catalog replacement updates hints") {
    CommandCatalogSnapshot empty; auto entries=build_settings_catalog(empty);
    CHECK(std::ranges::all_of(entries,[](const auto& s){return s.command_template.empty();}));
    CHECK_FALSE(setting_unavailable(get(entries,"camera.move_speed"),{}).empty());
    CHECK(get(entries,"preview.time_seconds").command.empty());
    Fixture f; auto catalog=f.model->command_catalog();
    auto it=std::ranges::find(catalog.commands,std::string("editor.camera.set"),&CommandDescriptor::name);
    REQUIRE(it!=catalog.commands.end()); it->help->at("fields")["/properties/move_speed"]["description"]="Fresh registry explanation";
    entries=build_settings_catalog(catalog); CHECK(get(entries,"camera.move_speed").help.find("Fresh registry explanation")!=std::string::npos);
}
TEST_CASE("settings help search and templates preserve workspace drafts scene files history and tasks") {
    Fixture f; REQUIRE(f.model->select(f.model->snapshot()->scene.entities().front().id));
    f.model->draft()->entity.name="保留草稿"; f.model->draft()->modified=true;
    f.model->environment_draft()->value["enabled"]=true; f.model->environment_draft()->modified=true;
    const auto state=f.model->snapshot()->state; const auto* snapshot=f.model->snapshot();
    const auto environment=f.model->environment_draft()->value; const auto history=f.model->history();
    const auto camera=f.model->editor_viewport()->query_camera().value();
    const auto bytes=read_file_bytes(f.files.root/"scene.json").value();
    const auto before=f.model->execute_console("tasks.list").value();
    const auto control=f.model->execute_console("tasks.list").value();
    for(int i=0;i<10;++i) { auto entries=build_settings_catalog(f.model->command_catalog()); CHECK_FALSE(search_settings(entries,"相机").indices.empty()); }
    const auto after=f.model->execute_console("tasks.list").value(); REQUIRE(after.size()-control.size()==control.size()-before.size());
    for(std::size_t i=0;i<control.size()-before.size();++i) CHECK(after[control.size()+i]["method"]==control[before.size()+i]["method"]);
    CHECK(f.model->snapshot()==snapshot); CHECK(f.model->snapshot()->state.revision==state.revision);
    CHECK(f.model->snapshot()->state.dirty==state.dirty);
    CHECK(f.model->draft()->entity.name=="保留草稿"); CHECK(f.model->environment_draft()->value==environment);
    CHECK(f.model->history().undo_count==history.undo_count); CHECK(f.model->history().redo_count==history.redo_count);
    CHECK(f.model->editor_viewport()->query_camera()->viewport.guard==camera.viewport.guard);
    CHECK(f.model->editor_viewport()->camera().same_session(camera.camera)); CHECK(read_file_bytes(f.files.root/"scene.json").value()==bytes);
}

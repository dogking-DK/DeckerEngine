#include "SettingsHelpSmoke.hpp"
#include "WorkbenchUi.hpp"
#include <cstdio>
#include <algorithm>
namespace dk::editor::detail {
namespace {
void require(bool value,const std::string& message) { if(!value) throw std::runtime_error("Settings help smoke: "+message); }
}
SettingsHelpSmoke::~SettingsHelpSmoke() {
    if(installed_) { auto& p=ImGui::GetPlatformIO(); p.Platform_GetClipboardTextFn=clipboard_get_; p.Platform_SetClipboardTextFn=clipboard_set_; p.Platform_ClipboardUserData=clipboard_user_; }
}
void SettingsHelpSmoke::initialize(Workspace& model,WorkbenchUi& ui) {
    state_=model.snapshot()->state; camera_=model.editor_viewport()->camera(); guard_=model.editor_viewport()->query()->guard;
    history_=ui.console_input().history_size(); baseline_tasks_=model.execute_console("tasks.list").value(); control_tasks_=model.execute_console("tasks.list").value();
    auto& p=ImGui::GetPlatformIO(); clipboard_get_=p.Platform_GetClipboardTextFn; clipboard_set_=p.Platform_SetClipboardTextFn; clipboard_user_=p.Platform_ClipboardUserData;
    p.Platform_ClipboardUserData=this;
    p.Platform_GetClipboardTextFn=[](ImGuiContext*) { return static_cast<SettingsHelpSmoke*>(ImGui::GetPlatformIO().Platform_ClipboardUserData)->clipboard_.c_str(); };
    p.Platform_SetClipboardTextFn=[](ImGuiContext*,const char* text) { static_cast<SettingsHelpSmoke*>(ImGui::GetPlatformIO().Platform_ClipboardUserData)->clipboard_=text; };
    installed_=true;
    const auto click=[&](std::string name,std::function<void()> verify={}) { actions_.push_back({"click",std::move(name),std::move(verify)}); };
    const auto search=[&](std::string query,std::string id) {
        click("settings_open"); actions_.push_back({"all",{},{}}); actions_.push_back({"text",std::move(query),{}});
        click("settings_first",[&,id]{ require(ui.settings_help().selected()==id,"wrong search result for "+id); });
    };
    const auto locate=[&](std::string id,std::string hint={}) {
        click("settings_locate",[&,id] { require(ui.settings_help().located()==id,"did not locate "+id); });
        actions_.push_back({"hover",id,[&,id,hint] { require(ui.settings_help().tooltip().starts_with(id),"missing tooltip for "+id); if(!hint.empty()) require(ui.settings_help().tooltip().find(hint)!=std::string::npos,"missing disabled reason/help: "+hint); }});
    };
    const auto ledger=[&] {
        const auto tasks=model.execute_console("tasks.list").value();
        require(tasks.size()-control_tasks_.size()==control_tasks_.size()-baseline_tasks_.size(),"help dispatched a task");
        for(std::size_t i=0;i<tasks.size()-control_tasks_.size();++i)
            require(tasks[control_tasks_.size()+i]["method"]==control_tasks_[baseline_tasks_.size()+i]["method"],"help changed task sequence");
    };
    search("相机 视场角","camera.fov_y_deg"); locate("camera.fov_y_deg","unit: deg"); click("camera_cancel");
    search("camera.near_plane","camera.near_plane"); locate("camera.near_plane","Automatic clipping");
    click("camera_auto_clip");
    actions_.push_back({"hover","camera.near_plane",[&] { require(ui.settings_help().tooltip().starts_with("camera.near_plane") && ui.settings_help().tooltip().find("Automatic clipping")==std::string::npos,"disabled help ignored the popup lens draft"); }});
    click("camera_cancel");
    search("path.temporal_reuse","path.temporal_reuse");
    locate("path.temporal_reuse",model.editor_viewport()->query()->capabilities.pathtracing ? "ReSTIR" : "unavailable");
    search("simulation.gravity_y","simulation.gravity_y"); locate("simulation.gravity_y","m/s2");
    search("preview.time_seconds","preview.time_seconds");
    click("settings_copy",[&] { require(clipboard_=="original test clipboard","invented command for GUI-only preview"); });
    click("settings_insert",[&] { require(ui.console_input().text().empty(),"invented preview template"); });
    locate("preview.time_seconds");
    search("camera.move_speed","camera.move_speed");
    click("settings_copy",[&] { inserted_=clipboard_; require(inserted_.starts_with("editor.camera.set ") && inserted_.find("<view_id>")!=std::string::npos,"missing guarded template"); require(ui.console_input().text().empty(),"copy changed Console"); });
    click("settings_insert",[&] { require(ui.console_input().text()==inserted_,"template not inserted"); });
    search("camera.fov_y_deg","camera.fov_y_deg");
    click("settings_insert",[&] { require(ui.settings_help().is_open() && ui.console_input().text()==inserted_,"existing Console text overwritten"); });
    click("settings_locate"); click("camera_cancel");
    search("inspector.translation","inspector.translation");
    click("settings_locate",[&,ledger] { require(!model.selection() && ui.settings_help().located()=="inspector.translation","location selected an entity"); ledger(); });
    click("entity"); click("name"); actions_.push_back({"all",{},{}});
    actions_.push_back({"text","保留 Inspector 草稿",[&] { require(model.draft() && model.draft()->modified,"name did not create draft"); baseline_tasks_=model.execute_console("tasks.list").value(); control_tasks_=model.execute_console("tasks.list").value(); }});
    search("云 覆盖率","environment.clouds.coverage"); locate("environment.clouds.coverage","Inspector draft");
    // Search remains open for the final capture; the pending Inspector and Console text survive.
    search("相机 视场角","camera.fov_y_deg");
    actions_.push_back({"finish",{},[&,ledger] {
        require(model.draft() && model.draft()->entity.name=="保留 Inspector 草稿","lost Inspector draft");
        require(ui.console_input().text()==inserted_,"lost Console input");
        require(model.history().undo_count==0 && model.history().redo_count==0,"help changed Undo");
        ledger();
        require(!model.simulation_state().run,"help started simulation");
        std::printf("Settings help smoke passed: search, locate, tooltip, disabled reason, copy, insert, draft/input/scene/task preservation\n"); done_=true;
    }});
}
void SettingsHelpSmoke::input(Workspace& model,WorkbenchUi& ui) {
    auto& io=ImGui::GetIO(); io.ConfigInputTrickleEventQueue=false;
    if(done_ || tick_++<12) return;
    if(actions_.empty()) initialize(model,ui);
    const auto stage=(tick_-13)/8,phase=(tick_-13)%8;
    require(stage<actions_.size(),"unexpected action index"); const auto& a=actions_[stage];
    if(phase==0) {
        action_time_=ImGui::GetTime(); io.AddFocusEvent(true);
        if(a.kind=="click") { auto it=ui.controls().find(a.text); require(it!=ui.controls().end(),"missing control "+a.text); pointer_=it->second; io.AddMousePosEvent(pointer_.x,pointer_.y); io.AddMouseButtonEvent(0,true); }
        else if(a.kind=="hover") { auto it=ui.settings_help().positions().find(a.text); require(it!=ui.settings_help().positions().end(),"missing help target "+a.text); pointer_=it->second; }
        else if(a.kind=="all") { io.AddKeyEvent(ImGuiMod_Ctrl,true); io.AddKeyEvent(ImGuiKey_A,true); }
        else if(a.kind=="text") io.AddInputCharactersUTF8(a.text.c_str());
    }
    io.AddMousePosEvent(pointer_.x,pointer_.y);
    if(phase==1) { io.AddMouseButtonEvent(0,false); io.AddKeyEvent(ImGuiKey_A,false); io.AddKeyEvent(ImGuiMod_Ctrl,false); }
    if(phase!=5) return;
    if(ImGui::GetTime()-action_time_<(a.kind=="hover" ? .65 : .16)) { --tick_; return; }
    require(model.snapshot()->state.document_id==state_.document_id && model.snapshot()->state.revision==state_.revision,"help mutated Scene");
    require(model.editor_viewport()->query()->guard==guard_ && model.editor_viewport()->camera().same_session(camera_),"help mutated camera/render settings");
    require(ui.console_input().history_size()==history_ && ui.errors()==0,"help submitted Console/error");
    if(a.verify) a.verify();
    std::printf("Settings help smoke stage %u %s %s passed\n",stage,a.kind.c_str(),a.text.c_str());
}
}

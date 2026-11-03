#include "ConsoleWorkflowSmoke.hpp"
#include "WorkbenchUi.hpp"
#include "EditorSupport.hpp"
#include <dk/io/File.hpp>
#include <imgui_internal.h>
#include <cstdio>
namespace dk::editor::detail {
namespace {
void require(bool value,const std::string& message) { if(!value) throw std::runtime_error("Console workflow: "+message); }
}
ConsoleWorkflowSmoke::ConsoleWorkflowSmoke(std::filesystem::path root):directory_(root/".dk-console-workflow") { std::filesystem::create_directories(directory_); }
void ConsoleWorkflowSmoke::initialize(WorkbenchUi& ui,Workspace& model) {
    simulation_camera_=ui.simulation_camera().revision();
    const auto action=[&](std::string kind,std::string text={},std::function<void()> check={}) { actions_.push_back({std::move(kind),std::move(text),std::move(check)}); };
    const auto search=[&](std::string query,std::string id) {
        action("click","settings_open"); action("all"); action("text",query);
        action("click","settings_first",[&,id] { require(ui.settings_help().selected()==id,"wrong settings result"); });
        action("click","settings_locate",[&,id] { require(ui.settings_help().located()==id,"failed to locate setting"); });
    };
    const auto field=[&](const char* control,double expected) {
        action("click",control,[&,expected] {
            const auto* state=ImGui::GetInputTextState(ImGui::GetActiveID());
            require(state && state->TextA.Data,"numeric GUI input not active"); gui_value_=state->TextA.Data;
            require(std::stod(gui_value_)==expected,"GUI value differs from command state: "+gui_value_);
        });
    };
    action("checkpoint","baseline");
    action("click","console_input"); action("text","editor.camera.se");
    action("tab",{},[&] { require(ui.console_input().popup_open(),"command completion did not open"); });
    action("tab",{},[&] { require(ui.console_input().text()=="editor.camera.set" && ui.console_input().history_size()==0,"acceptance executed or changed command"); });
    action("text"," fov_y_deg=",[&] { require(ui.console_input().assistance().hint.find("unit: deg")!=std::string::npos,"camera parameter help missing"); });
    action("camera_args",{},[&] { require(model.editor_viewport()->camera().settings().lens.fov_y!=35,"typing changed lens before Run"); });
    action("click","console_run",[&] { require(ui.console_input().text().empty() && ui.console_input().history_size()==1,"explicit Run failed"); });
    action("checkpoint","console_lens");
    search("相机 视场角","camera.fov_y_deg"); field("camera_fov",35);
    action("checkpoint","panel_sync"); // Keep the popup open to test the example's preflight rejection.
    action("click","camera_cancel"); action("click","camera_settings"); field("camera_fov",35);
    action("all"); action("text","45"); action("enter"); action("click","camera_apply");
    action("checkpoint","gui_lens");
    action("click","console_input"); action("legacy"); action("enter");
    action("checkpoint","legacy_json");
    action("click","console_input"); action("viewport_args");
    action("tab",{},[&] { require(ui.console_input().popup_open(),"renderer enum completion did not open"); });
    action("tab",{},[&] { require(ui.console_input().text().ends_with("\"pathtrace\"") && !model.editor_viewport()->settings().path_trace && ui.console_input().history_size()==2,"enum acceptance executed or altered history"); });
    action("text"," target_spp=2 direct_lighting=classic"); action("enter");
    action("checkpoint","path_console");
    search("path.target_spp","path.target_spp"); field("path_target",2);
    action("checkpoint","path_panel");
    action("checkpoint","ipc_example");
    action("finish");
}
void ConsoleWorkflowSmoke::input(WorkbenchUi& ui,Workspace& model) {
    auto& io=ImGui::GetIO(); io.ConfigInputTrickleEventQueue=false;
    if(done_ || warmup_++<12) return;
    if(actions_.empty()) initialize(ui,model);
    if(waiting_) {
        const auto next=directory_/(actions_[stage_].text+".continue");
        if(!std::filesystem::exists(next)) return;
        // Windows can briefly expose the renamed file before a sharing handle closes.
        // Poll only this test acknowledgement; the host's acceptance deadline is bounded.
        const auto bytes=read_file_bytes(next); if(!bytes) return;
        guard_=Json::parse(reinterpret_cast<const char*>(bytes->data()),reinterpret_cast<const char*>(bytes->data()+bytes->size())).at("guard");
        waiting_=false; ready_=false; tick_=0; ++stage_;
    }
    require(stage_<actions_.size(),"unexpected stage"); const auto& a=actions_[stage_];
    if(a.kind=="checkpoint") { ready_=true; return; }
    if(a.kind=="finish") { done_=true; return; }
    const auto phase=tick_++;
    if(phase==0) {
        action_time_=ImGui::GetTime(); io.AddFocusEvent(true);
        if(a.kind=="click") {
            const auto it=ui.controls().find(a.text); require(it!=ui.controls().end(),"missing control "+a.text);
            pointer_=it->second; io.AddMousePosEvent(pointer_.x,pointer_.y); io.AddMouseButtonEvent(0,true);
        } else if(a.kind=="all") { io.AddKeyEvent(ImGuiMod_Ctrl,true); io.AddKeyEvent(ImGuiKey_A,true); }
        else if(a.kind=="enter") io.AddKeyEvent(ImGuiKey_Enter,true);
        else if(a.kind=="tab") io.AddKeyEvent(ImGuiKey_Tab,true);
        else {
            auto text=a.text;
            // Test client explicitly copies guards read over IPC; the product never fills them.
            if(a.kind=="camera_args") text="35 guard="+guard_.dump();
            if(a.kind=="viewport_args") text="editor.viewport.set guard="+guard_.dump()+" renderer=pa";
            if(a.kind=="legacy") text=Json{{"method","editor.camera.set"},{"params",{{"guard",guard_},{"fov_y_deg",35}}}}.dump();
            io.AddInputCharactersUTF8(text.c_str());
        }
    }
    io.AddMousePosEvent(pointer_.x,pointer_.y);
    if(phase==1) { io.AddMouseButtonEvent(0,false); for(auto key:{ImGuiKey_A,ImGuiKey_Tab,ImGuiKey_Enter}) io.AddKeyEvent(key,false); io.AddKeyEvent(ImGuiMod_Ctrl,false); }
    if(phase<5) return;
    if(ImGui::GetTime()-action_time_<.16) { --tick_; return; }
    require(ui.errors()==0,"unexpected UI error");
    if(a.verify) a.verify();
    events_.push_back({{"stage",stage_},{"action",a.kind},{"text",a.text},{"console",ui.console_input().text()},{"history",ui.console_input().history_size()}});
    std::printf("Console workflow stage %u %s passed\n",stage_,a.kind.c_str());
    ++stage_; tick_=0;
}
void ConsoleWorkflowSmoke::presented(WorkbenchUi& ui,Workspace& model,Viewport& viewport) {
    if(!ready_ || waiting_) return;
    const auto camera=ui.input().camera();
    if(!viewport.current(model.snapshot()->state) || !viewport.camera_current(camera) || !viewport.error().empty()) return;
    if(model.editor_viewport()->settings().path_trace && (!viewport.published_path() || viewport.path_samples()!=2)) return;
    require(ui.errors()==0 && !model.dirty() && model.snapshot()->state.revision==1 && model.history().undo_count==0 && model.history().redo_count==0,"settings changed Scene/Undo or raised an error");
    require(!model.simulation_state().run && ui.simulation_camera().revision()==simulation_camera_,"workflow changed Simulation");
    const auto& name=actions_[stage_].text;
    auto report=Json{{"checkpoint",name},{"camera",take(model.execute_console("editor.camera.query"))},{"viewport",take(model.execute_console("editor.viewport.query"))},
        {"scene",{{"revision",model.snapshot()->state.revision},{"dirty",model.dirty()},{"undo",model.history().undo_count},{"redo",model.history().redo_count}}},
        {"console_history",ui.console_input().history_size()},{"gui_value",gui_value_},{"events",events_},
        {"published_camera_revision",viewport.published_camera().revision()},{"simulation_camera_revision",simulation_camera_},
        {"pixels",viewport.published_path() ? viewport.capture_path_pixels() : viewport.pixel_signature()}};
    const auto bytes=viewport.retained_pixels(); const auto& info=viewport.info();
    std::string ppm="P6\n"+std::to_string(info.width)+" "+std::to_string(info.height)+"\n255\n";
    for(std::size_t i=0;i<bytes.size();i+=4) for(unsigned c=0;c<3;++c) ppm+=char(std::to_integer<unsigned char>(bytes[i+c]));
    check(write_file_bytes_atomic(directory_/(name+".ppm"),std::as_bytes(std::span(ppm.data(),ppm.size()))));
    const auto text=report.dump(2); check(write_file_bytes_atomic(directory_/(name+".json"),std::as_bytes(std::span(text.data(),text.size()))));
    std::printf("Console workflow checkpoint %s passed\n",name.c_str()); std::fflush(stdout); waiting_=true;
}
}

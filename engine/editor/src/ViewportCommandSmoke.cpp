#include "ViewportCommandSmoke.hpp"
#include "WorkbenchUi.hpp"
#include "EditorSupport.hpp"
#include <dk/io/File.hpp>
#include <cstdio>
namespace dk::editor::detail {
namespace {
constexpr const char* names[]={"raster","path_ipc","exposure_gui","pause_console","reset_ipc","resume_ipc","restir_gui","medium_ipc","failed","recovered","active_close"};
}
ViewportCommandSmoke::ViewportCommandSmoke(std::filesystem::path root):directory_(root/".dk-viewport") { std::filesystem::create_directories(directory_); }
void ViewportCommandSmoke::input(WorkbenchUi& ui,Workspace& model) {
    if(waiting_) {
        if(stage_==10 || !std::filesystem::exists(directory_/(std::string(names[stage_])+".continue"))) return;
        ++stage_; tick_=0; waiting_=false; ready_=false;
    }
    auto& io=ImGui::GetIO(); io.AddFocusEvent(true);
    auto click=[&](const char* id) {
        const auto it=ui.controls().find(id); if(it==ui.controls().end()) throw std::runtime_error(std::string("Missing viewport control: ")+id);
        pointer_=it->second; io.AddMousePosEvent(pointer_.x,pointer_.y); io.AddMouseButtonEvent(0,true);
    };
    if(stage_==1) {
        if(tick_==0) click("path_settings");
        if(tick_==1) io.AddMouseButtonEvent(0,false);
    } else if(stage_==2 || stage_==3) {
        if(tick_==0) click(stage_==2 ? "path_exposure" : "console_input");
        if(tick_==1) io.AddMouseButtonEvent(0,false);
        if(tick_==2) { io.AddKeyEvent(ImGuiMod_Ctrl,true); io.AddKeyEvent(ImGuiKey_A,true); }
        if(tick_==3) { io.AddKeyEvent(ImGuiKey_A,false); io.AddKeyEvent(ImGuiMod_Ctrl,false); }
        if(tick_==4) {
            const auto q=take(model.execute_console("editor.viewport.query"));
            const auto text=stage_==2 ? std::string("1") : "editor.viewport.pause guard="+q.at("guard").dump();
            io.AddInputCharactersUTF8(text.c_str());
        }
        if(tick_==6) io.AddKeyEvent(ImGuiKey_Enter,true);
        if(tick_==7) io.AddKeyEvent(ImGuiKey_Enter,false);
    } else if(stage_==6) {
        if(tick_==0) click("path_restir");
        if(tick_==1) io.AddMouseButtonEvent(0,false);
    }
    io.AddMousePosEvent(pointer_.x,pointer_.y);
    ready_=++tick_>=12;
}
void ViewportCommandSmoke::presented(WorkbenchUi& ui,Workspace& model,Viewport& viewport) {
    if(waiting_ || !ready_) return;
    const auto progress=viewport.path_status().progress;
    const auto state=take(model.editor_viewport()->query());
    if(stage_==8) { if(viewport.error().empty() || viewport.current(model.snapshot()->state)) return; }
    else if(stage_==4) { if(progress.completed_spp || !progress.paused) return; }
    else if(stage_==10) { if(!progress.pending || progress.completed_spp>=1000000) return; }
    else {
        if(!viewport.current(model.snapshot()->state) || !viewport.camera_current(ui.input().camera()) || !viewport.error().empty()) return;
        if(stage_ && (!viewport.published_path() || viewport.path_samples()!=(stage_>=7 ? 1u : stage_>=5 ? 3u : 2u))) return;
        if(stage_==2 && state.settings.path.exposure!=1) return;
        if(stage_==3 && (!state.settings.path.paused || !progress.paused || !ui.console_input().text().empty())) return;
        if(stage_==6 && !viewport.path_info().restir) return;
        if(stage_==7 && !viewport.path_info().medium) return;
    }
    if(ui.errors()!=(stage_>=8 ? 1u : 0u)) throw std::runtime_error("Unexpected viewport command smoke UI errors");
    const auto signature=viewport.published_path() ? viewport.capture_path_pixels() : viewport.pixel_signature();
    auto report=take(model.execute_console("editor.viewport.query"));
    report["checkpoint"]=names[stage_]; report["pixels"]=signature;
    report["native"]={{"path_mode",ui.path_mode()},{"paused",state.settings.path.paused},{"exposure",ui.path_settings().exposure},
        {"generation",progress.generation},{"completed_spp",progress.completed_spp},{"pending",progress.pending},
        {"scene_revision",model.snapshot()->state.revision},{"undo",model.history().undo_count},{"dirty",model.dirty()},
        {"console_history",ui.console_input().history_size()},{"ui_errors",ui.errors()}};
    const auto text=report.dump(2);
    check(write_file_bytes_atomic(directory_/(std::string(names[stage_])+".json"),std::as_bytes(std::span(text.data(),text.size()))));
    if(stage_==6) {
        const auto bytes=viewport.retained_pixels(); const auto& info=viewport.info();
        std::string ppm="P6\n"+std::to_string(info.width)+" "+std::to_string(info.height)+"\n255\n";
        for(std::size_t i=0;i<bytes.size();i+=4) for(unsigned c=0;c<3;++c) ppm+=char(std::to_integer<unsigned char>(bytes[i+c]));
        check(write_file_bytes_atomic(directory_/"restir.ppm",std::as_bytes(std::span(ppm.data(),ppm.size()))));
    }
    std::printf("Viewport command checkpoint %s passed\n",names[stage_]); std::fflush(stdout); waiting_=true;
}
}

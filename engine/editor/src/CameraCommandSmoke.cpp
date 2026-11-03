#include "CameraCommandSmoke.hpp"
#include "WorkbenchUi.hpp"
#include "EditorSupport.hpp"
#include <dk/io/File.hpp>
#include <cstdio>
namespace dk::editor::detail {
namespace {
constexpr const char* names[]={"raster","orbit_ipc","orthographic_console","lens_gui","draft_gui","cancel_gui",
    "frame_ipc","reset_gui","path_ipc","speed_gui","pan_console","gesture_gui","released","active_close"};
}
CameraCommandSmoke::CameraCommandSmoke(std::filesystem::path root):directory_(root/".dk-camera") { std::filesystem::create_directories(directory_); }
void CameraCommandSmoke::input(WorkbenchUi& ui,Workspace& model) {
    auto& io=ImGui::GetIO(); io.AddFocusEvent(true);
    if(waiting_) {
        // Keep the synthetic held gesture stable while the IPC client inspects it.
        // The SDL backend otherwise replaces the test pointer with the physical cursor.
        if(stage_==11) { io.AddMousePosEvent(pointer_.x,pointer_.y); io.AddMouseButtonEvent(ImGuiMouseButton_Right,true); }
        if(stage_==13 || !std::filesystem::exists(directory_/(std::string(names[stage_])+".continue"))) return;
        ++stage_; tick_=0; waiting_=false; ready_=false;
    }
    auto click=[&](const char* name,int button=0) {
        const auto it=ui.controls().find(name); if(it==ui.controls().end()) throw std::runtime_error(std::string("Missing camera control: ")+name);
        pointer_=it->second; io.AddMousePosEvent(pointer_.x,pointer_.y); io.AddMouseButtonEvent(button,true);
    };
    auto select=[&](unsigned tick) {
        if(tick_==tick) { io.AddKeyEvent(ImGuiMod_Ctrl,true); io.AddKeyEvent(ImGuiKey_A,true); }
        if(tick_==tick+1) { io.AddKeyEvent(ImGuiKey_A,false); io.AddKeyEvent(ImGuiMod_Ctrl,false); }
    };
    if(stage_==2 || stage_==10) {
        if(tick_==0) click("console_input");
        if(tick_==1) io.AddMouseButtonEvent(0,false);
        select(2);
        if(tick_==4) {
            const auto q=take(model.execute_console("editor.camera.query"));
            const auto text=std::string(stage_==2 ? "editor.camera.set projection=orthographic ortho_height=3 standard_view=front guard=" :
                "editor.camera.pan right=0.2 up=0.1 guard=")+q.at("guard").dump();
            io.AddInputCharactersUTF8(text.c_str());
        }
        if(tick_==6) io.AddKeyEvent(ImGuiKey_Enter,true);
        if(tick_==7) io.AddKeyEvent(ImGuiKey_Enter,false);
    } else if(stage_==3 || stage_==4 || stage_==9) {
        if(tick_==0) click("camera_settings");
        if(tick_==1) io.AddMouseButtonEvent(0,false);
        if(tick_==3) click(stage_==9 ? "camera_speed" : "camera_ortho_height");
        if(tick_==4) io.AddMouseButtonEvent(0,false);
        select(5);
        if(tick_==7) io.AddInputCharactersUTF8(stage_==9 ? "9" : stage_==4 ? "6" : "4");
        if(tick_==9) io.AddKeyEvent(ImGuiKey_Enter,true);
        if(tick_==10) io.AddKeyEvent(ImGuiKey_Enter,false);
        if(stage_!=4 && tick_==12) click("camera_apply");
        if(stage_!=4 && tick_==13) io.AddMouseButtonEvent(0,false);
    } else if(stage_==5 || stage_==7) {
        if(tick_==0) click(stage_==5 ? "camera_cancel" : "camera_reset");
        if(tick_==1) io.AddMouseButtonEvent(0,false);
    } else if(stage_==11) {
        // Leave the Console text field before starting the held navigation gesture.
        if(tick_==0) click("viewport_empty");
        if(tick_==1) io.AddMouseButtonEvent(0,false);
        if(tick_==4) click("viewport",ImGuiMouseButton_Right);
        if(tick_>=4) io.AddMouseButtonEvent(ImGuiMouseButton_Right,true);
        if(tick_==6) { pointer_.x+=20; pointer_.y+=8; }
    } else if(stage_==12 && tick_==0) io.AddMouseButtonEvent(ImGuiMouseButton_Right,false);
    io.AddMousePosEvent(pointer_.x,pointer_.y); ready_=++tick_>=18;
}
void CameraCommandSmoke::presented(WorkbenchUi& ui,Workspace& model,Viewport& viewport) {
    if(waiting_ || !ready_) return;
    const auto camera=ui.input().camera(); const auto progress=viewport.path_status().progress;
    if(stage_==11 && tick_==60) {
        const auto& io=ImGui::GetIO();
        std::printf("Camera gesture waiting: navigating=%d right=%d text=%d focusLost=%d current=%d cameraCurrent=%d spp=%u error=%s\n",
            ui.input().navigating(),io.MouseDown[1],io.WantTextInput,io.AppFocusLost,
            viewport.current(model.snapshot()->state),viewport.camera_current(camera),viewport.path_samples(),viewport.error().c_str());
        std::fflush(stdout);
    }
    if(stage_==13) { if(!progress.pending || progress.completed_spp>=1000000) return; }
    else if(!viewport.current(model.snapshot()->state) || !viewport.camera_current(camera) || !viewport.error().empty()) return;
    if(stage_>=8 && stage_<13 && (!viewport.published_path() || viewport.path_samples()!=2)) return;
    if(stage_==3 && camera.settings().lens.ortho_height!=4) return;
    if(stage_==9 && camera.settings().navigation.move_speed!=9) return;
    if(stage_==11 && !ui.input().navigating()) return;
    if(stage_==12 && ui.input().navigating()) return;
    if(ui.errors()) throw std::runtime_error("Unexpected camera command smoke UI errors");
    auto report=take(model.execute_console("editor.camera.query"));
    if(stage_==4 && report["blocked_reason"].get<std::string>().find("Camera")==std::string::npos) return;
    if(stage_==5 && !report["blocked_reason"].get<std::string>().empty()) return;
    report["checkpoint"]=names[stage_];
    report["pixels"]=viewport.published_path() ? viewport.capture_path_pixels() : viewport.pixel_signature();
    const auto shown=viewport.published_camera().pose();
    report["native"]={{"render_revision",camera.revision()},{"published_camera_revision",viewport.published_camera().revision()},
        {"published_eye",{shown.eye.x(),shown.eye.y(),shown.eye.z()}},{"path_generation",progress.generation},
        {"path_spp",progress.completed_spp},{"pending",progress.pending},{"navigating",ui.input().navigating()},
        {"scene_revision",model.snapshot()->state.revision},{"dirty",model.dirty()},{"undo",model.history().undo_count},
        {"simulation_camera_revision",ui.simulation_camera().revision()},{"console_history",ui.console_input().history_size()}};
    if(stage_==2 || stage_==8) {
        const auto bytes=viewport.retained_pixels(); const auto& info=viewport.info();
        std::string ppm="P6\n"+std::to_string(info.width)+" "+std::to_string(info.height)+"\n255\n";
        for(std::size_t i=0;i<bytes.size();i+=4) for(unsigned c=0;c<3;++c) ppm+=char(std::to_integer<unsigned char>(bytes[i+c]));
        check(write_file_bytes_atomic(directory_/(std::string(names[stage_])+".ppm"),std::as_bytes(std::span(ppm.data(),ppm.size()))));
    }
    const auto text=report.dump(2);
    check(write_file_bytes_atomic(directory_/(std::string(names[stage_])+".json"),std::as_bytes(std::span(text.data(),text.size()))));
    std::printf("Camera command checkpoint %s passed\n",names[stage_]); std::fflush(stdout); waiting_=true;
}
}

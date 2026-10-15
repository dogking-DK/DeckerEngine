#include "PathSmoke.hpp"
#include "EditorSupport.hpp"
#include <dk/io/File.hpp>
#include <array>
namespace dk::editor::detail {
namespace {
constexpr std::array names={"path","paused","exposure","reset","resumed","camera","gizmo","external","resize","minimize","raster","returned","reloaded","closing"};
void require(bool value,const char* message) { if(!value) throw std::runtime_error(std::string{"Path smoke: "}+message); }
std::span<const char* const> actions(std::size_t stage) {
    static constexpr std::array path={"path_mode","path_target",":all",":two",":enter"};
    static constexpr std::array pause={"path_pause","path_target",":all",":three",":enter"};
    static constexpr std::array resume={"path_target",":all",":two",":enter","path_pause"}; static constexpr std::array reset={"path_reset"}; static constexpr std::array exposure={"path_exposure",":all",":two",":enter"};
    static constexpr std::array orbit={":orbit",":drag",":release"}; static constexpr std::array gizmo={"viewport","gizmo_0",":drag",":release"};
    static constexpr std::array resize={":resize"},minimize={":minimize"},raster={"path_raster"},returned={"path_mode"};
    static constexpr std::array closing={"path_target",":all",":many",":enter","path_reset"};
    switch(stage) {
    case 0:return path; case 1:return pause; case 4:return resume; case 2:return exposure; case 3:return reset;
    case 5:return orbit; case 6:return gizmo; case 8:return resize; case 9:return minimize;
    case 10:return raster; case 11:return returned; case 13:return closing; default:return {};
    }
}
}
PathSmoke::PathSmoke(const std::filesystem::path& root):directory_(root/".dk-path") {
    require(std::filesystem::create_directory(directory_),"checkpoint directory must be fresh");
}
void PathSmoke::poll_window(platform::Window& window,bool minimized) {
    if(restore_==std::chrono::steady_clock::time_point{}) return;
    if(minimized) ++minimized_;
    if(std::chrono::steady_clock::now()>=restore_) { check(window.restore()); restore_={}; }
}
void PathSmoke::input(const WorkbenchUi& ui,const Workspace& model,const Viewport& viewport,platform::Window& window) {
    if(done_) return;
    if(waiting_) {
        if(!std::filesystem::is_regular_file(directory_/(std::string{names[stage_]}+".continue"))) return;
        waiting_=false; ready_=false; tick_=0;
        if(++stage_==names.size()) { done_=true; return; }
    }
    auto& io=ImGui::GetIO(); io.ConfigInputTrickleEventQueue=false;
    if(tick_++<12) return;
    const auto sequence=actions(stage_); const auto elapsed=tick_-13,step=elapsed/8,phase=elapsed%8;
    if(step>=sequence.size()) { ready_=true; return; }
    const std::string_view action=sequence[step];
    if(action=="gizmo_0" && phase==0 && (!viewport.current(model.snapshot()->state) || !viewport.camera_current(ui.input().camera()))) { --tick_; return; }
    if(phase==0) {
        io.AddFocusEvent(true);
        if(action==":all") { io.AddKeyEvent(ImGuiMod_Ctrl,true); io.AddKeyEvent(ImGuiKey_A,true); }
        else if(action==":two") io.AddInputCharactersUTF8("2");
        else if(action==":three") io.AddInputCharactersUTF8("3");
        else if(action==":many") io.AddInputCharactersUTF8("64");
        else if(action==":enter") io.AddKeyEvent(ImGuiKey_Enter,true);
        else if(action==":drag") { pointer_.x+=40; pointer_.y+=stage_==5 ? 15 : 0; }
        else if(action==":release") { io.AddMouseButtonEvent(0,false); io.AddMouseButtonEvent(1,false); }
        else if(action==":resize") check(window.resize(1280,800));
        else if(action==":minimize") { check(window.minimize()); restore_=std::chrono::steady_clock::now()+std::chrono::milliseconds(150); }
        else {
            const auto control=ui.controls().find(action==":orbit" ? "viewport" : std::string{action});
            require(control!=ui.controls().end(),"missing GUI control"); pointer_=control->second;
            io.AddMousePosEvent(pointer_.x,pointer_.y); io.AddMouseButtonEvent(action==":orbit" ? 1 : 0,true);
        }
    }
    io.AddMousePosEvent(pointer_.x,pointer_.y);
    if(phase==1) {
        if(action==":all") { io.AddKeyEvent(ImGuiKey_A,false); io.AddKeyEvent(ImGuiMod_Ctrl,false); }
        else if(action==":enter") io.AddKeyEvent(ImGuiKey_Enter,false);
        else if(!action.starts_with(":") && action!="gizmo_0") io.AddMouseButtonEvent(0,false);
    }
}
void PathSmoke::presented(const Workspace& model,const WorkbenchUi& ui,Viewport& viewport) {
    if(done_ || waiting_ || !ready_) return;
    require(viewport.error().empty(),viewport.error().c_str());
    require(ui.errors()==0,"unexpected GUI error");
    const auto progress=viewport.path_status().progress;
    if(stage_==3) { if(progress.completed_spp || !progress.paused) return; }
    else if(stage_==13) { if(!progress.pending || progress.completed_spp>=64) return; }
    else {
        if(!viewport.current(model.snapshot()->state) || !viewport.camera_current(ui.input().camera())) return;
        if(stage_!=10 && (progress.completed_spp!=2 || viewport.path_samples()!=2)) return;
    }
    require(stage_!=9 || (minimized_>0 && restore_==std::chrono::steady_clock::time_point{}),"minimized loop was not exercised");
    const auto& state=model.snapshot()->state;
    std::uint64_t pixels=0;
    if(stage_!=3 && stage_!=13) pixels=stage_==10 ? viewport.pixel_signature() : viewport.capture_path_pixels();
    Json report{{"checkpoint",names[stage_]},{"document_id",state.document_id.to_string()},{"revision",state.revision},
        {"generation",progress.generation},{"target",progress.target_spp},{"spp",progress.completed_spp},{"paused",progress.paused},{"pending",progress.pending},
        {"exposure",ui.path_settings().exposure},{"pixels",pixels},{"width",viewport.info().width},{"height",viewport.info().height},
        {"camera",ui.input().camera().revision()},{"undo",model.history().undo_count},{"path_mode",ui.path_mode()}};
    if(stage_==0 || stage_==4 || stage_==5) {
        const auto& camera=ui.input().camera(); const auto world=camera.world().matrix();
        const Vec3d eye=world.block<3,1>(0,3),target=eye-world.block<3,1>(0,2),up=world.block<3,1>(0,1);
        report["capture_camera"]={{"eye",{eye.x(),eye.y(),eye.z()}},{"target",{target.x(),target.y(),target.z()}},
            {"up",{up.x(),up.y(),up.z()}},{"fov_y",65},{"near",camera.near_plane()},{"far",camera.far_plane()}};
        const auto& settings=ui.path_settings();
        report["pathtrace"]={{"version",1},{"target_spp",2},{"seed",settings.seed},{"max_bounces",settings.bounces},
            {"rr_start",std::min(3u,settings.bounces)},{"environment_rgb",{settings.sky.x(),settings.sky.y(),settings.sky.z()}},
            {"sun",settings.sun},{"surface_only",settings.surface_only},{"exposure_ev",settings.exposure}};
        Json linear=Json::array(); for(const auto& v:viewport.path_linear()) linear.push_back({v.x(),v.y(),v.z()});
        require(linear.size()==std::size_t{viewport.info().width}*viewport.info().height,"missing completed linear image");
        const auto data=linear.dump();
        check(write_file_bytes_atomic(directory_/(std::string{names[stage_]}+".linear.json"),std::as_bytes(std::span{data.data(),data.size()})));
    }
    const auto text=report.dump(2); check(write_file_bytes_atomic(directory_/(std::string{names[stage_]}+".json"),std::as_bytes(std::span{text.data(),text.size()})));
    waiting_=true;
}
}

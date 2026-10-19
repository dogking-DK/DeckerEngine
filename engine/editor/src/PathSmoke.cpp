#include "PathSmoke.hpp"
#include "EditorSupport.hpp"
#include <dk/io/File.hpp>
#include <array>
#include <SDL3/SDL.h>
#include <cmath>
#include <format>
namespace dk::editor::detail {
namespace {
constexpr std::array names={"path","paused","exposure","reset","resumed","camera","gizmo","external","resize","minimize","raster","returned","reloaded","orthographic","ortho_height","perspective","closing"};
constexpr std::array media_names={"surface","air","cloud","time","exposure","rejected","rejected_again","recovered","surface_again","media_again","reloaded","console","orthographic","ortho_height","perspective","closing"};
constexpr std::array desktop_names={"path","atmosphere","air_motion","clouds_enabled","clouds","cloud_motion","rejected","recovered","underground","camera_recovered","returned","media_air","media_air_motion","media_cloud","media_cloud_motion"};
void require(bool value,const char* message) { if(!value) throw std::runtime_error(std::string{"Path smoke: "}+message); }
std::span<const char* const> actions(std::size_t stage,bool desktop,bool media) {
    static constexpr std::array desktop_path={"path_mode","path_target",":all",":one",":enter"};
    static constexpr std::array desktop_air={"path_raster","environment_open","environment_enabled","environment_apply","camera_fly"};
    static constexpr std::array desktop_clouds={"cloud_preset_sparse","environment_apply"};
    static constexpr std::array desktop_return={"path_settings","path_surface","path_mode","camera_walk"};
    static constexpr std::array underground={"camera_orbit",":orbit",":down",":release"};
    static constexpr std::array home={":home"};
    static constexpr std::array orthographic={"camera_orthographic"}, perspective={"camera_perspective"};
    static constexpr std::array height={"camera_settings","camera_ortho_height",":all",":three",":enter","camera_apply","path_settings"};
    if(media) {
        static constexpr std::array media_orthographic={"camera_orthographic","path_raster","path_mode"};
        static constexpr std::array air={"path_media","environment_open","environment_enabled","environment_apply","path_settings"};
        static constexpr std::array time={"path_time",":all",":two",":enter"}, exposure={"path_exposure",":all",":two",":enter"};
        static constexpr std::array surface={"path_surface"}, medium={"path_media"};
        static constexpr std::array console={"path_refresh","console_input",":all",":command",":enter"};
        static constexpr std::array closing={"path_target",":all",":many",":enter","path_reset"};
        switch(stage) { case 0:return desktop_path; case 1:return air; case 3:return time; case 4:return exposure;
            case 8:return surface; case 9:return medium; case 11:return console; case 12:return media_orthographic; case 13:return height; case 14:return perspective; case 15:return closing; default:return {}; }
    }
    if(desktop) { if(stage==0) return desktop_path; if(stage==1) return desktop_air; if(stage==4) return desktop_clouds;
        if(stage==8) return underground; if(stage==9) return home; if(stage==10) return desktop_return;
        static constexpr std::array media_mode={"path_settings","path_media"}; if(stage==11) return media_mode; return {}; }
    static constexpr std::array path={"path_mode","camera_settings","camera_fov",":all",":bad_fov",":enter","camera_apply",
        "camera_fov",":all",":fov",":enter","camera_apply","path_settings","path_target",":all",":two",":enter"};
    static constexpr std::array pause={"path_pause","camera_fly","camera_walk","camera_orbit","camera_settings","camera_speed",":all",":speed",":enter","camera_apply","path_settings","path_target",":all",":three",":enter"};
    static constexpr std::array resume={"path_target",":all",":two",":enter","path_pause"}; static constexpr std::array reset={"path_reset"}; static constexpr std::array exposure={"path_exposure",":all",":two",":enter"};
    static constexpr std::array orbit={"camera_fly",":orbit",":forward",":release","camera_orbit"}; static constexpr std::array gizmo={"viewport","gizmo_0",":drag",":release"};
    static constexpr std::array resize={":resize"},minimize={":minimize"},raster={"path_raster"},returned={"path_mode"};
    static constexpr std::array closing={"path_target",":all",":many",":enter","path_reset"};
    switch(stage) {
    case 0:return path; case 1:return pause; case 4:return resume; case 2:return exposure; case 3:return reset;
    case 5:return orbit; case 6:return gizmo; case 8:return resize; case 9:return minimize;
    case 10:return raster; case 11:return returned; case 13:return orthographic; case 14:return height; case 15:return perspective; case 16:return closing; default:return {};
    }
}
}
PathSmoke::PathSmoke(const std::filesystem::path& root,bool desktop,bool media):directory_(root/".dk-path"),desktop_(desktop),media_(media) {
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
        if(!std::filesystem::is_regular_file(directory_/(std::string{media_ ? media_names[stage_] : desktop_ ? desktop_names[stage_] : names[stage_]}+".continue"))) return;
        waiting_=false; ready_=false; tick_=0;
        motion_start_={}; motion_frames_=motion_updates_=0; motion_samples_=Json::array();
        if(++stage_==(media_ ? media_names.size() : desktop_ ? desktop_names.size() : names.size())) { done_=true; return; }
    }
    auto& io=ImGui::GetIO(); io.ConfigInputTrickleEventQueue=false;
    if(tick_++<12) return;
    if(desktop_ && (stage_==2 || stage_==5 || stage_==12 || stage_==14)) {
        if(ready_) return;
        if(motion_start_==std::chrono::steady_clock::time_point{}) {
            motion_start_=std::chrono::steady_clock::now(); pointer_=ui.controls().at("viewport");
            motion_origin_=pointer_;
            motion_signature_=stage_>=11 ? viewport.info().frame : viewport.pixel_signature(); io.AddFocusEvent(true);
            io.AddMousePosEvent(pointer_.x,pointer_.y); io.AddMouseButtonEvent(1,true);
        } else if(std::chrono::steady_clock::now()-motion_start_<std::chrono::seconds(2)) {
            const double seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-motion_start_).count();
            const float next=motion_origin_.x+float(8*std::sin(seconds*12.566370614359));
            if(ui.input().camera().navigation_mode()!=NavigationMode::orbit) {
                require(SDL_GetWindowRelativeMouseMode(take(window.native_sdl_window())),"Desktop navigation did not capture relative mouse");
                SDL_Event e{}; e.type=SDL_EVENT_MOUSE_MOTION; e.motion.windowID=SDL_GetWindowID(take(window.native_sdl_window()));
                e.motion.xrel=next-pointer_.x; require(SDL_PushEvent(&e),"Desktop relative motion injection failed");
            } else io.AddMousePosEvent(next,pointer_.y);
            pointer_.x=next;
        } else { io.AddMouseButtonEvent(1,false); ready_=true; }
        return;
    }
    const auto sequence=actions(stage_,desktop_,media_); const auto elapsed=tick_-13,step=elapsed/8,phase=elapsed%8;
    if(step>=sequence.size()) { ready_=true; return; }
    const std::string_view action=sequence[step];
    if(action=="gizmo_0" && phase==0 && (!viewport.current(model.snapshot()->state) || !viewport.camera_current(ui.input().camera()))) { --tick_; return; }
    if(phase==0) {
        io.AddFocusEvent(true);
        if(action==":all") { io.AddKeyEvent(ImGuiMod_Ctrl,true); io.AddKeyEvent(ImGuiKey_A,true); }
        else if(action==":command") {
            const auto bytes=take(read_file_bytes(directory_/"command.json")); require(bytes.size()<=8192,"console fixture exceeds limit");
            const std::string text(reinterpret_cast<const char*>(bytes.data()),bytes.size()); io.AddInputCharactersUTF8(text.c_str());
        }
        else if(action==":bad_fov") io.AddInputCharactersUTF8("0");
        else if(action==":fov") io.AddInputCharactersUTF8("55");
        else if(action==":speed") io.AddInputCharactersUTF8("9");
        else if(action==":one") io.AddInputCharactersUTF8("1");
        else if(action==":two") io.AddInputCharactersUTF8("2");
        else if(action==":three") io.AddInputCharactersUTF8("3");
        else if(action==":many") io.AddInputCharactersUTF8("64");
        else if(action==":enter") io.AddKeyEvent(ImGuiKey_Enter,true);
        else if(action==":home") io.AddKeyEvent(ImGuiKey_Home,true);
        else if(action==":down") pointer_.y-=80;
        else if(action==":forward") { motion_start_=std::chrono::steady_clock::now(); motion_signature_=viewport.published_camera().revision();
            io.AddKeyEvent(ImGuiMod_Ctrl,true); io.AddKeyEvent(ImGuiKey_W,true);
            SDL_Event e{}; e.type=SDL_EVENT_MOUSE_MOTION; e.motion.windowID=SDL_GetWindowID(take(window.native_sdl_window())); e.motion.xrel=2; require(SDL_PushEvent(&e),"relative injection failed"); }
        else if(action==":drag") { pointer_.x+=40; pointer_.y+=stage_==5 ? 15 : 0; }
        else if(action==":release") { io.AddMouseButtonEvent(0,false); io.AddMouseButtonEvent(1,false); io.AddKeyEvent(ImGuiKey_W,false); io.AddKeyEvent(ImGuiKey_S,false); io.AddKeyEvent(ImGuiMod_Ctrl,false); }
        else if(action==":resize") check(window.resize(1280,800));
        else if(action==":minimize") { check(window.minimize()); restore_=std::chrono::steady_clock::now()+std::chrono::milliseconds(150); }
        else {
            const auto control=ui.controls().find(action==":orbit" ? "viewport" : std::string{action});
            require(control!=ui.controls().end(),"missing GUI control"); pointer_=control->second;
            io.AddMousePosEvent(pointer_.x,pointer_.y); io.AddMouseButtonEvent(action==":orbit" ? 1 : 0,true);
        }
    }
    if(action==":forward" && phase==1) {
        const auto ms=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-motion_start_).count();
        if(ms<800) { const bool forward=(ms/80)%2==0; io.AddKeyEvent(ImGuiKey_W,forward); io.AddKeyEvent(ImGuiKey_S,!forward); --tick_; }
        else { io.AddKeyEvent(ImGuiKey_W,false); io.AddKeyEvent(ImGuiKey_S,false); }
    }
    io.AddMousePosEvent(pointer_.x,pointer_.y);
    if(!desktop_ && !media_ && stage_==0 && step==6 && phase==4)
        require(ui.input().camera().fov_y()==65 && ui.controls().contains("camera_fov"),"Invalid camera Apply changed lens or discarded its draft");
    if(phase==1) {
        if(action==":all") { io.AddKeyEvent(ImGuiKey_A,false); io.AddKeyEvent(ImGuiMod_Ctrl,false); }
        else if(action==":enter") io.AddKeyEvent(ImGuiKey_Enter,false);
        else if(action==":home") io.AddKeyEvent(ImGuiKey_Home,false);
        else if(!action.starts_with(":") && action!="gizmo_0") io.AddMouseButtonEvent(0,false);
    }
}
void PathSmoke::presented(const Workspace& model,const WorkbenchUi& ui,Viewport& viewport) {
    if(!desktop_ && !media_ && stage_==5 && !waiting_ && motion_start_!=std::chrono::steady_clock::time_point{}) {
        const auto revision=viewport.published_camera().revision();
        if(revision!=motion_signature_) { motion_signature_=revision; ++motion_updates_; }
    }
    if(media_) { presented_media(model,ui,viewport); return; }
    if(desktop_ && !waiting_ && !ready_ && motion_start_!=std::chrono::steady_clock::time_point{}) {
        ++motion_frames_;
        const auto signature=stage_>=11 ? viewport.info().frame : viewport.pixel_signature();
        if(signature!=motion_signature_) {
            ++motion_updates_; motion_signature_=signature;
            if(stage_>=11) motion_samples_.push_back({{"latency_ms",viewport.path_latency_ms()},
                {"width",viewport.info().width},{"height",viewport.info().height},{"camera",viewport.published_camera().revision()}});
        }
    }
    if(done_ || waiting_ || !ready_) return;
    require(viewport.path_unavailable().empty(),viewport.path_unavailable().c_str());
    const bool rejected=desktop_ && (stage_==6 || stage_==8);
    if(rejected && viewport.error().empty()) return;
    if(!rejected) require(viewport.error().empty(),viewport.error().c_str());
    require(ui.errors()==(desktop_ && stage_>=8 ? 2u : desktop_ && stage_>=6 ? 1u : 0u),"unexpected GUI error");
    const auto progress=viewport.path_status().progress;
    if(desktop_) {
        const bool raster=stage_>0 && stage_<10;
        if(!rejected && (!viewport.current(model.snapshot()->state) || !viewport.camera_current(ui.input().camera()))) return;
        if(stage_>=3 && stage_<10 && !rejected && viewport.environment_warmup()<32) return;
        if(!raster && (progress.completed_spp!=1 || viewport.path_samples()!=1)) return;
        require(ui.path_mode()!=raster,"desktop render mode did not change");
        require(stage_==0 || model.snapshot()->scene.environment().enabled,"atmosphere was not enabled through GUI");
        require(stage_<3 || stage_==11 || stage_==12 || model.snapshot()->scene.environment().clouds.enabled,"clouds were not enabled");
        if(stage_==2 || stage_==5 || stage_==12 || stage_==14) require(motion_updates_>=2,"continuous camera motion starved completed images");
        if(stage_>=11) {
            if(viewport.path_interactive() || std::max(viewport.info().width,viewport.info().height)<=192) return;
            require(viewport.path_info().medium.has_value(),"Scene media not enabled");
        }
        const auto signature=raster ? viewport.pixel_signature() : viewport.capture_path_pixels();
        const auto name=desktop_names[stage_]; const auto& info=viewport.info();
        Json report{{"checkpoint",name},{"spp",raster ? 0 : progress.completed_spp},{"width",info.width},{"height",info.height},
            {"pixels",signature},{"revision",info.revision},{"scene_revision",model.snapshot()->state.revision},{"path_mode",ui.path_mode()},
            {"error",viewport.error()},{"motion_samples",motion_samples_},{"motion_frames",motion_frames_},{"motion_updates",motion_updates_},{"motion_seconds",2}};
        const auto bytes=viewport.retained_pixels();
        std::string ppm="P6\n"+std::to_string(info.width)+" "+std::to_string(info.height)+"\n255\n";
        for(std::size_t i=0;i<bytes.size();i+=4) for(unsigned c=0;c<3;++c) ppm.push_back(char(std::to_integer<unsigned char>(bytes[i+c])));
        check(write_file_bytes_atomic(directory_/(std::string{name}+".ppm"),std::as_bytes(std::span{ppm.data(),ppm.size()})));
        const auto text=report.dump(2); check(write_file_bytes_atomic(directory_/(std::string{name}+".json"),std::as_bytes(std::span{text.data(),text.size()})));
        waiting_=true; return;
    }
    if(stage_==3) { if(progress.completed_spp || !progress.paused) return; }
    else if(stage_==16) { if(!progress.pending || progress.completed_spp>=64) return; }
    else {
        if(!viewport.current(model.snapshot()->state) || !viewport.camera_current(ui.input().camera())) return;
        if(stage_!=10 && (progress.completed_spp!=2 || viewport.path_samples()!=2)) return;
    }
    require(stage_!=9 || (minimized_>0 && restore_==std::chrono::steady_clock::time_point{}),"minimized loop was not exercised");
    const auto& state=model.snapshot()->state;
    if(stage_==5) require(motion_updates_>=2,"Fly motion starved completed path images");
    if(stage_==1) require(ui.input().camera().settings().navigation.move_speed==9,"Camera speed preference was not applied");
    std::uint64_t pixels=0;
    if(stage_!=3 && stage_!=16) pixels=stage_==10 ? viewport.pixel_signature() : viewport.capture_path_pixels();
    Json report{{"checkpoint",names[stage_]},{"document_id",state.document_id.to_string()},{"revision",state.revision},
        {"generation",progress.generation},{"target",progress.target_spp},{"spp",progress.completed_spp},{"paused",progress.paused},{"pending",progress.pending},
        {"exposure",ui.path_settings().exposure},{"pixels",pixels},{"width",viewport.info().width},{"height",viewport.info().height},
        {"navigation_updates",motion_updates_},{"camera",ui.input().camera().revision()},{"undo",model.history().undo_count},{"path_mode",ui.path_mode()}};
    if(stage_==0 || stage_==4 || stage_==5 || (stage_>=13 && stage_<=15)) {
        const auto& camera=ui.input().camera(); const auto world=camera.world().matrix();
        const Vec3d eye=world.block<3,1>(0,3),target=eye-world.block<3,1>(0,2),up=world.block<3,1>(0,1);
        report["capture_camera"]={{"eye",{eye.x(),eye.y(),eye.z()}},{"target",{target.x(),target.y(),target.z()}},
            {"up",{up.x(),up.y(),up.z()}},{"fov_y",camera.fov_y()},{"near",camera.near_plane()},{"far",camera.far_plane()}};
        if(camera.projection_mode()==ProjectionMode::orthographic) {
            report["capture_camera"].erase("fov_y"); report["capture_camera"]["projection"]="orthographic";
            report["capture_camera"]["ortho_height"]=camera.settings().lens.ortho_height;
        } else report["capture_camera"]["projection"]="perspective";
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
void PathSmoke::presented_media(const Workspace& model,const WorkbenchUi& ui,Viewport& viewport) {
    if(done_ || waiting_ || !ready_) return;
    require(ui.path_mode(),"unsupported raster transition left Scene media mode");
    const bool rejected=stage_==5 || stage_==6,closing=stage_==15;
    require(viewport.path_unavailable().empty(),viewport.path_unavailable().c_str());
    if(rejected && viewport.error().empty()) return;
    if(!rejected) require(viewport.error().empty(),viewport.error().c_str());
    require(ui.errors()==(stage_>=6 ? 2u : stage_==5 ? 1u : 0u),"unexpected media GUI error");
    const auto progress=viewport.path_status().progress;
    if(closing) { if(!progress.pending || progress.completed_spp>=64) return; }
    else if(!rejected && (!viewport.current(model.snapshot()->state) || !viewport.camera_current(ui.input().camera()) ||
        progress.completed_spp!=1 || viewport.path_samples()!=1)) return;
    const auto& state=model.snapshot()->state; const auto& used=viewport.path_info(); const auto& settings=ui.path_settings();
    const auto& camera=viewport.published_camera(); const auto world=camera.world().matrix();
    const Vec3d eye=world.block<3,1>(0,3),target=eye-world.block<3,1>(0,2),up=world.block<3,1>(0,1);
    Json report{{"checkpoint",media_names[stage_]},{"document_id",state.document_id.to_string()},{"revision",state.revision},
        {"shown_revision",used.revision},{"current",viewport.current(state)},{"generation",progress.generation},
        {"spp",progress.completed_spp},{"pending",progress.pending},{"exposure",settings.exposure},
        {"width",viewport.info().width},{"height",viewport.info().height},{"error",viewport.error()},
        {"mode",used.medium ? "scene_media" : "surface_only"},{"model_version",used.medium ? 4 : 3},{"sampler_version",used.medium ? 2 : 1},
        {"field_hash",std::format("fnv1a64:{:016x}",used.medium ? used.medium->field_hash : 0)},
        {"geometry_fingerprint",std::format("fnv1a64:{:016x}",used.geometry_fingerprint)},
        {"input_fingerprint",std::format("fnv1a64:{:016x}",used.fingerprint)},
        {"time_seconds",used.medium ? used.medium->medium.time_seconds : 0},
        {"pixels",closing ? 0 : viewport.capture_path_pixels()},
        {"capture_camera",{{"eye",{eye.x(),eye.y(),eye.z()}},{"target",{target.x(),target.y(),target.z()}},
            {"up",{up.x(),up.y(),up.z()}},{"fov_y",camera.fov_y()},{"near",camera.near_plane()},{"far",camera.far_plane()}}},
        {"pathtrace",{{"version",2},{"medium_mode",settings.scene_media ? "scene_media" : "surface_only"},{"target_spp",1},
            {"seed",settings.seed},{"max_bounces",settings.bounces},{"rr_start",std::min(3u,settings.bounces)},
            {"environment_rgb",{settings.sky.x(),settings.sky.y(),settings.sky.z()}},{"sun",settings.sun},{"exposure_ev",settings.exposure}}}};
    if(camera.projection_mode()==ProjectionMode::orthographic) {
        report["capture_camera"].erase("fov_y"); report["capture_camera"]["projection"]="orthographic";
        report["capture_camera"]["ortho_height"]=camera.settings().lens.ortho_height;
    } else report["capture_camera"]["projection"]="perspective";
    if(settings.scene_media) report["pathtrace"].update({{"time_seconds",settings.time_seconds},{"cloud_albedo",settings.cloud_albedo},{"cloud_anisotropy",settings.cloud_anisotropy}});
    if(!closing) {
        Json linear=Json::array(); for(const auto& v:viewport.path_linear()) linear.push_back({v.x(),v.y(),v.z()});
        require(linear.size()==std::size_t{viewport.info().width}*viewport.info().height,"missing media linear image");
        const auto data=linear.dump(); check(write_file_bytes_atomic(directory_/(std::string{media_names[stage_]}+".linear.json"),std::as_bytes(std::span{data.data(),data.size()})));
    }
    const auto text=report.dump(2); check(write_file_bytes_atomic(directory_/(std::string{media_names[stage_]}+".json"),std::as_bytes(std::span{text.data(),text.size()})));
    waiting_=true;
}
}

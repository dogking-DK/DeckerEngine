#include "NavigationSmoke.hpp"
#include "EditorSupport.hpp"
#include <SDL3/SDL.h>
#include <imgui_internal.h>
#include <array>
#include <cstdio>

namespace dk::editor::detail {
namespace {
void require(bool value,const char* message) { if(!value) throw std::runtime_error(std::string("Navigation smoke: ")+message); }
}
void NavigationSmoke::poll(platform::Window& window,bool minimized) {
    if(restore_==std::chrono::steady_clock::time_point{}) return;
    if(minimized) ++minimized_;
    if(std::chrono::steady_clock::now()>=restore_) { check(window.restore()); restore_={}; }
}
bool NavigationSmoke::input(Workspace& model,WorkbenchUi& ui,platform::Window& window,Viewport& viewport) {
    constexpr std::array actions={"camera_fly",":fly",":escape",":mode","camera_fly",":focus",":minimize",":hidden",":collapsed",":resize",":popup",
        "camera_cancel",":text",":blur",":close","cancel","revert","camera_walk",":walk","camera_orbit","camera_reset"};
    if(stage_>=actions.size()) return true;
    const std::string_view action=actions[stage_];
    const auto phase=phase_++;
    auto& io=ImGui::GetIO(); io.ConfigInputTrickleEventQueue=false;
    auto* native=take(window.native_sdl_window());
    const auto control=[&](const char* key) { const auto found=ui.controls().find(key); require(found!=ui.controls().end(),key); return found->second; };
    const auto click=[&](const char* key) { pointer_=control(key); io.AddMousePosEvent(pointer_.x,pointer_.y); io.AddMouseButtonEvent(0,true); };
    const auto event=[&](SDL_EventType type) { SDL_Event e{}; e.type=type; e.window.windowID=SDL_GetWindowID(native); require(SDL_PushEvent(&e),"native event injection failed"); };
    const auto& camera=ui.input().camera();
    const bool motion=action==":fly" || action==":walk";
    const bool capture=action.starts_with(":") && action!=":blur";
    if(phase==0) {
        std::printf("Navigation begin %s\n",actions[stage_]);
        if(stage_==0) { RelativeMouse invalid{nullptr}; require(!invalid.acquire(&invalid) && !invalid.owns(&invalid),"failed mouse acquire published an owner"); }
        io.AddFocusEvent(true); pose_=camera.world().matrix(); camera_revision_=camera.revision();
        scene_revision_=model.snapshot()->state.revision; history_=model.history().undo_count; dirty_=model.snapshot()->state.dirty;
        if(capture) {
            if(action==":close") { require(model.draft()!=nullptr,"close needs a selected draft"); model.draft()->modified=true; }
            pointer_=control("viewport_empty"); io.AddMousePosEvent(pointer_.x,pointer_.y);
            io.AddMouseButtonEvent(1,true); io.AddKeyEvent(ImGuiKey_W,true);
            if(action==":walk") io.AddKeyEvent(ImGuiKey_E,true);
            start_=std::chrono::steady_clock::now(); updates_=0; published_=viewport.published_camera().revision();
        } else if(action==":blur") { io.AddKeyEvent(ImGuiKey_Escape,true); }
        else click(actions[stage_]);
    }
    io.AddMousePosEvent(pointer_.x,pointer_.y);
    if(phase==1 && !capture) { io.AddMouseButtonEvent(0,false); io.AddKeyEvent(ImGuiKey_Escape,false); }
    if(capture && phase==2) require(ui.input().navigating() && SDL_GetWindowRelativeMouseMode(native),"RMB did not acquire SDL relative mode");
    if(motion && phase>=2 && phase<=4) {
        if(viewport.published_camera().revision()!=published_) { published_=viewport.published_camera().revision(); ++updates_; }
        if(std::chrono::steady_clock::now()-start_<std::chrono::milliseconds(700)) {
            SDL_Event e{}; e.type=SDL_EVENT_MOUSE_MOTION; e.motion.windowID=SDL_GetWindowID(native);
            e.motion.xrel=1; e.motion.yrel=action==":walk" ? -2.f : .25f;
            require(SDL_PushEvent(&e),"relative motion injection failed");
            if(phase==4) --phase_;
        } else {
            require(updates_>=3,"continuous navigation starved actual image publications");
            require(camera.revision()!=camera_revision_ && camera.world().matrix()!=pose_,"WASD/look did not move camera");
            if(action==":walk") require(camera.world().matrix()(1,3)==pose_(1,3),"Walk changed world height");
            std::printf("Navigation %s continuous publications=%u\n",actions[stage_],updates_);
            io.AddKeyEvent(ImGuiKey_W,false); io.AddKeyEvent(ImGuiKey_E,false); phase_=5;
        }
    }
    if(capture && !motion && phase==3) {
        if(action==":escape") io.AddKeyEvent(ImGuiKey_Escape,true);
        else if(action==":mode") click("camera_walk");
        else if(action==":focus") event(SDL_EVENT_WINDOW_FOCUS_LOST);
        else if(action==":hidden") event(SDL_EVENT_WINDOW_HIDDEN);
        else if(action==":collapsed") ImGui::FindWindowByName("Viewport")->HiddenFramesCanSkipItems=5;
        else if(action==":minimize") { check(window.minimize()); restore_=std::chrono::steady_clock::now()+std::chrono::milliseconds(120); minimized_=0; }
        else if(action==":resize") check(window.resize(1320,820));
        else if(action==":popup") click("camera_settings");
        else if(action==":text") click("console_input");
        else if(action==":close") event(SDL_EVENT_WINDOW_CLOSE_REQUESTED);
    }
    if(phase==5) { stopped_=camera.world().matrix(); io.AddMouseButtonEvent(0,false); }
    if(phase==7 && capture) {
        require(camera.world().matrix()==stopped_,"camera kept moving after key release/cancellation");
        if(motion) require(ui.input().navigating() && SDL_GetWindowRelativeMouseMode(native),"key release unexpectedly ended mouse look");
        else require(!ui.input().navigating() && !SDL_GetWindowRelativeMouseMode(native),"cancellation retained SDL relative mode");
        if(action==":minimize") require(minimized_>0,"minimized loop was not exercised");
        io.AddMouseButtonEvent(1,false); io.AddKeyEvent(ImGuiKey_W,false); io.AddKeyEvent(ImGuiKey_E,false); io.AddKeyEvent(ImGuiKey_Escape,false);
        io.AddFocusEvent(true);
    }
    if(phase<11) return false;
    require(!ui.input().navigating() && !SDL_GetWindowRelativeMouseMode(native),"mouse release did not end capture");
    require(model.snapshot()->state.revision==scene_revision_ && model.history().undo_count==history_ && model.snapshot()->state.dirty==dirty_,"navigation changed scene/history");
    if(action=="camera_fly" || action=="camera_walk" || action=="camera_orbit") {
        require(camera.world().matrix()==pose_ && camera.revision()==camera_revision_,"navigation mode changed pose/revision");
        require(camera.navigation_mode()==(action=="camera_fly" ? NavigationMode::fly : action=="camera_walk" ? NavigationMode::walk : NavigationMode::orbit),"mode radio failed");
    }
    if(motion && !viewport.camera_current(camera)) return false;
    if(motion) require(camera.world().matrix()==stopped_,"button release left movement active");
    std::printf("Navigation stage %u %s passed\n",stage_,actions[stage_]);
    ++stage_; phase_=0; return false;
}
}

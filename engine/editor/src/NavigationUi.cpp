#include "NavigationUi.hpp"
#include <SDL3/SDL.h>
#include <cstdio>

namespace dk::editor::detail {
RelativeMouse::~RelativeMouse() { if(owner_) release(owner_); }
Result<void> RelativeMouse::acquire(const void* owner) {
    if(owner_ && owner_!=owner) return std::unexpected(Error{ErrorCode::invalid_state,"Another viewport owns mouse navigation"});
    if(owner_==owner) return {};
    SDL_GetMouseState(&restore_x_,&restore_y_);
    if(!SDL_SetWindowRelativeMouseMode(window_,true))
        return std::unexpected(Error{ErrorCode::internal_error,std::string("Relative mouse: ")+SDL_GetError()});
    delta_.setZero(); owner_=owner; return {};
}
void RelativeMouse::release(const void* owner) {
    if(owner_!=owner || !owner_) return;
    // Never move another application's cursor after focus loss or during background automation.
    if(SDL_GetMouseFocus()==window_ && SDL_GetKeyboardFocus()==window_) SDL_WarpMouseInWindow(window_,restore_x_,restore_y_);
    if(!SDL_SetWindowRelativeMouseMode(window_,false)) {
        std::fprintf(stderr,"Relative mouse release: %s\n",SDL_GetError());
        delta_.setZero(); return; // retain owner so cancellation/destruction retries
    }
    owner_=nullptr; delta_.setZero();
}
bool RelativeMouse::belongs(const SDL_Event& event) const {
    return event.type>=SDL_EVENT_WINDOW_FIRST && event.type<=SDL_EVENT_WINDOW_LAST && event.window.windowID==SDL_GetWindowID(window_);
}
bool RelativeMouse::event(const SDL_Event& event) {
    if(!owner_ || event.type!=SDL_EVENT_MOUSE_MOTION || event.motion.windowID!=SDL_GetWindowID(window_)) return false;
    // SDL window coordinates already match ImGui's logical coordinates. No framebuffer scaling.
    delta_+=Vec2d{event.motion.xrel,event.motion.yrel}; return true;
}
Vec2d RelativeMouse::consume(const void* owner) {
    if(owner_!=owner) return Vec2d::Zero();
    const Vec2d result=delta_; delta_.setZero(); return result;
}
void NavigationUi::cancel() { mouse_.release(this); navigation_.cancel(); button_=-1; last_={}; }
void NavigationUi::toolbar(Camera& camera,std::map<std::string,ImVec2>& controls,const char* prefix) {
    ImGui::PushID(prefix);
    const char* names[]={"Orbit","Fly","Walk"};
    const char* keys[]={"orbit","fly","walk"};
    for(int i=0;i<3;++i) {
        const auto mode=static_cast<NavigationMode>(i);
        ImGui::BeginDisabled(camera.projection_mode()==ProjectionMode::orthographic && mode!=NavigationMode::orbit);
        if(ImGui::RadioButton(names[i],camera.navigation_mode()==mode)) {
            cancel(); const auto result=camera.set_navigation_mode(mode);
            if(!result) error_=result.error().message;
        }
        const auto a=ImGui::GetItemRectMin(),b=ImGui::GetItemRectMax();
        controls[std::string(prefix)+keys[i]]={(a.x+b.x)/2,(a.y+b.y)/2};
        ImGui::EndDisabled();
        if(i<2) ImGui::SameLine();
    }
    if(camera.navigation_mode()==NavigationMode::orbit) {
        ImGui::TextDisabled("RMB: orbit | MMB: pan | wheel: zoom");
        ImGui::TextDisabled("F: frame | Home: reset");
    }
    else {
        ImGui::TextDisabled("Hold RMB: look + WASD | %s | Shift/Ctrl: fast/slow",camera.navigation_mode()==NavigationMode::fly ? "Q/E: down/up" : "fixed height");
        ImGui::TextDisabled("Wheel while held: speed %.3g m/s | Esc: release | F: frame | Home: reset",camera.settings().navigation.move_speed);
    }
    if(!error_.empty()) ImGui::TextWrapped("%s",error_.c_str());
    ImGui::PopID();
}
void NavigationUi::image(Camera& camera,bool blocked) {
    used_=true;
    auto& io=ImGui::GetIO();
    const auto minimum=ImGui::GetItemRectMin(),size=ImGui::GetItemRectSize();
    const bool popup=ImGui::IsPopupOpen(nullptr,ImGuiPopupFlags_AnyPopupId|ImGuiPopupFlags_AnyPopupLevel);
    if(blocked || io.AppFocusLost || io.WantTextInput || popup || ImGui::IsKeyPressed(ImGuiKey_Escape,false) ||
       (active() && camera.navigation_mode()==NavigationMode::orbit && (minimum.x!=minimum_.x || minimum.y!=minimum_.y || size.x!=size_.x || size.y!=size_.y))) { cancel(); return; }
    const bool hovered=ImGui::IsItemHovered(),orbit=camera.navigation_mode()==NavigationMode::orbit;
    if(active() && !ImGui::IsMouseDown(button_)) cancel();
    if(!active() && hovered) {
        int button=-1;
        if(ImGui::IsMouseClicked(ImGuiMouseButton_Right)) button=ImGuiMouseButton_Right;
        else if(orbit && ImGui::IsMouseClicked(ImGuiMouseButton_Middle)) button=ImGuiMouseButton_Middle;
        if(button>=0) {
            if(!orbit) {
                const auto result=mouse_.acquire(this);
                if(!result) { error_=result.error().message; return; }
                navigation_.begin();
            }
            button_=button; minimum_=minimum; size_=size; last_=std::chrono::steady_clock::now(); error_.clear();
        }
    }
    if(orbit) {
        if(active() && !ImGui::IsMouseClicked(button_)) {
            if(button_==ImGuiMouseButton_Right) camera.orbit(io.MouseDelta.x,io.MouseDelta.y);
            else camera.pan(io.MouseDelta.x,io.MouseDelta.y,size.y);
        }
        if(hovered && io.MouseWheel!=0) camera.dolly(io.MouseWheel);
    } else if(active()) {
        if(!mouse_.owns(this)) { cancel(); return; }
        const auto now=std::chrono::steady_clock::now();
        NavigationInput input;
        input.seconds=std::chrono::duration<double>(now-last_).count(); last_=now;
        input.look=mouse_.consume(this); input.wheel=io.MouseWheel;
        input.move={double(ImGui::IsKeyDown(ImGuiKey_D))-ImGui::IsKeyDown(ImGuiKey_A),
            double(ImGui::IsKeyDown(ImGuiKey_E))-ImGui::IsKeyDown(ImGuiKey_Q),
            double(ImGui::IsKeyDown(ImGuiKey_W))-ImGui::IsKeyDown(ImGuiKey_S)};
        input.fast=io.KeyShift; input.slow=io.KeyCtrl;
        if(const auto result=navigation_.update(camera,input); !result) { error_=result.error().message; cancel(); }
    }
}
}

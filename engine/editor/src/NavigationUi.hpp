#pragma once
#include <dk/editor/CameraNavigation.hpp>
#include <imgui.h>
#include <chrono>
#include <map>

struct SDL_Window;
union SDL_Event;
namespace dk::editor::detail {
// One main-thread owner for the borrowed window. Outlives both navigation adapters.
class RelativeMouse final {
public:
    explicit RelativeMouse(SDL_Window* window) : window_(window) {}
    ~RelativeMouse();
    RelativeMouse(const RelativeMouse&)=delete;
    RelativeMouse& operator=(const RelativeMouse&)=delete;
    [[nodiscard]] Result<void> acquire(const void* owner);
    void release(const void* owner);
    [[nodiscard]] bool event(const SDL_Event&); // true: relative motion consumed
    [[nodiscard]] bool belongs(const SDL_Event&) const;
    [[nodiscard]] Vec2d consume(const void* owner);
    [[nodiscard]] bool owns(const void* owner) const { return owner_==owner; }
private:
    SDL_Window* window_;
    const void* owner_=nullptr;
    Vec2d delta_=Vec2d::Zero();
    float restore_x_=0,restore_y_=0;
};
class NavigationUi final {
public:
    explicit NavigationUi(RelativeMouse& mouse) : mouse_(mouse) {}
    ~NavigationUi() { cancel(); }
    void cancel();
    void begin_frame() { used_=false; }
    void end_frame() { if(!used_) cancel(); }
    void toolbar(Camera&,std::map<std::string,ImVec2>&,const char* prefix);
    // Called immediately after the actual Image; remains usable when the last image is stale.
    void image(Camera&,bool blocked);
    [[nodiscard]] bool active() const { return button_>=0; }
private:
    RelativeMouse& mouse_;
    CameraNavigation navigation_;
    std::chrono::steady_clock::time_point last_{};
    int button_=-1;
    bool used_=false;
    ImVec2 minimum_{},size_{};
    std::string error_;
};
}

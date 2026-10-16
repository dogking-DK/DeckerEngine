#pragma once
#include "WorkbenchUi.hpp"
#include <dk/platform/Window.hpp>
#include <chrono>
namespace dk::editor::detail {
class PathSmoke final {
public:
    explicit PathSmoke(const std::filesystem::path&,bool desktop=false);
    void input(const WorkbenchUi&,const Workspace&,const Viewport&,platform::Window&);
    void presented(const Workspace&,const WorkbenchUi&,Viewport&);
    void poll_window(platform::Window&,bool);
    bool done() const { return done_; }
private:
    std::filesystem::path directory_;
    std::size_t stage_=0;
    unsigned tick_=0,minimized_=0;
    bool desktop_=false,waiting_=false,ready_=false,done_=false;
    ImVec2 pointer_{};
    std::chrono::steady_clock::time_point restore_{};
};
}

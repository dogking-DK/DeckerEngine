#pragma once
#include "WorkbenchUi.hpp"
#include <dk/platform/Window.hpp>

namespace dk::editor::detail {
class NavigationSmoke final {
public:
    bool input(Workspace&,WorkbenchUi&,platform::Window&,Viewport&);
    void poll(platform::Window&,bool minimized);
private:
    unsigned stage_=0,phase_=0,updates_=0,minimized_=0;
    std::uint64_t camera_revision_=0,published_=0,scene_revision_=0;
    std::size_t history_=0;
    bool dirty_=false;
    Mat4d pose_=Mat4d::Identity(),stopped_=Mat4d::Identity();
    std::chrono::steady_clock::time_point start_{},restore_{};
    ImVec2 pointer_{};
};
}

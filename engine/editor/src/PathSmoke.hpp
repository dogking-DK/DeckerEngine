#pragma once
#include "WorkbenchUi.hpp"
#include <dk/platform/Window.hpp>
#include <chrono>
namespace dk::editor::detail {
class PathSmoke final {
public:
    explicit PathSmoke(const std::filesystem::path&,bool desktop=false,bool media=false);
    void input(const WorkbenchUi&,const Workspace&,const Viewport&,platform::Window&);
    void presented(const Workspace&,const WorkbenchUi&,Viewport&);
    void poll_window(platform::Window&,bool);
    bool done() const { return done_; }
private:
    void presented_media(const Workspace&,const WorkbenchUi&,Viewport&);
    std::filesystem::path directory_;
    std::size_t stage_=0;
    unsigned tick_=0,minimized_=0;
    bool desktop_=false,media_=false,waiting_=false,ready_=false,done_=false;
    ImVec2 pointer_{};
    ImVec2 motion_origin_{};
    std::chrono::steady_clock::time_point restore_{};
    std::chrono::steady_clock::time_point motion_start_{};
    std::uint64_t motion_signature_=0;
    unsigned motion_frames_=0,motion_updates_=0;
    Json motion_samples_=Json::array();
};
}

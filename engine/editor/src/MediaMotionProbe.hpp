#pragma once
#include "WorkbenchUi.hpp"
#include <dk/platform/Window.hpp>
#include <unordered_map>

namespace dk::editor::detail {
// Explicit, bounded, disposable Sponza observation. Never enabled by ordinary GUI settings.
class MediaMotionProbe final {
public:
    explicit MediaMotionProbe(const std::filesystem::path& root);
    void input(const WorkbenchUi&,Workspace&,Viewport&,platform::Window&);
    void presented(const WorkbenchUi&,Viewport&);
    bool done() const { return stage_==4; }
private:
    using Clock=std::chrono::steady_clock;
    double now() const { return std::chrono::duration<double,std::milli>(Clock::now()-origin_).count(); }
    void save();
    std::filesystem::path directory_;
    Json config_,report_;
    Clock::time_point origin_=Clock::now();
    unsigned stage_=0,ticks_=0,images_=0;
    bool started_=false;
    double motion_start_=0,measure_start_=0,stop_=0,previous_present_=0,pending_input_=0,applied_input_=0,last_look_=0;
    std::uint64_t observed_camera_=0,published_camera_=0;
    std::uint32_t restored_spp_=0;
    ImVec2 pointer_{};
    std::unordered_map<std::uint64_t,double> input_times_;
};
}

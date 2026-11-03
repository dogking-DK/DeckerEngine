#pragma once
#include <dk/commands/Schema.hpp>
#include <filesystem>
#include <functional>
#include <imgui.h>
namespace dk::editor { class Workspace; namespace detail {
class WorkbenchUi; class Viewport;
class ConsoleWorkflowSmoke final {
public:
    explicit ConsoleWorkflowSmoke(std::filesystem::path);
    void input(WorkbenchUi&,Workspace&);
    void presented(WorkbenchUi&,Workspace&,Viewport&);
    bool done() const { return done_; }
private:
    struct Action { std::string kind,text; std::function<void()> verify; };
    void initialize(WorkbenchUi&,Workspace&);
    std::filesystem::path directory_;
    std::vector<Action> actions_;
    Json guard_,events_=Json::array();
    unsigned warmup_=0,stage_=0,tick_=0;
    std::uint64_t simulation_camera_=0;
    ImVec2 pointer_{};
    double action_time_=0;
    std::string gui_value_;
    bool ready_=false,waiting_=false,done_=false;
};
}}

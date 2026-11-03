#pragma once
#include <dk/editor/Workspace.hpp>
#include <imgui.h>
#include <functional>
namespace dk::editor::detail {
class WorkbenchUi;
class SettingsHelpSmoke final {
public:
    ~SettingsHelpSmoke();
    void input(Workspace&,WorkbenchUi&);
    bool done() const { return done_; }
private:
    struct Action { std::string kind,text; std::function<void()> verify; };
    void initialize(Workspace&,WorkbenchUi&);
    unsigned tick_=0;
    double action_time_=0;
    std::vector<Action> actions_;
    ImVec2 pointer_{};
    bool done_=false,installed_=false;
    std::string clipboard_="original test clipboard",inserted_;
    std::size_t history_=0;
    Json baseline_tasks_,control_tasks_;
    DocumentState state_;
    Camera camera_;
    EditorViewportGuard guard_;
    const char* (*clipboard_get_)(ImGuiContext*)=nullptr;
    void (*clipboard_set_)(ImGuiContext*,const char*)=nullptr;
    void* clipboard_user_=nullptr;
};
}

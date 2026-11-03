#pragma once
#include "ConsoleContextSmoke.hpp"
#include <imgui.h>
#include <cstdint>
#include <string>
namespace dk::editor {
class Workspace;
namespace detail {
class WorkbenchUi;
class ConsoleCompletionSmoke final {
public:
    ~ConsoleCompletionSmoke();
    bool input(Workspace&,WorkbenchUi&,unsigned window_id);
private:
    unsigned tick_=0, errors_=0;
    std::size_t history_=0, entities_=0;
    std::uint64_t revision_=0;
    ImVec2 pointer_{};
    std::string clipboard_;
    bool installed_=false;
    double action_time_=0;
    ConsoleContextSmoke context_;
    const char* (*clipboard_get_)(ImGuiContext*)=nullptr;
    void* clipboard_user_=nullptr;
    void restore();
};
}
}

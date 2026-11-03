#pragma once
#include "ConsoleCompletionSmoke.hpp"
#include <imgui.h>
#include <cstdint>
#include <string>

namespace dk::editor {
class Workspace;
namespace detail {
class WorkbenchUi;
// Driven through the same ImGui/SDL input path as the workbench smoke.
class ConsoleSmoke final {
public:
    ~ConsoleSmoke();
    bool input(Workspace&, WorkbenchUi&, unsigned window_id);
private:
    ConsoleCompletionSmoke completion_;
    void restore_clipboard();
    unsigned tick_ = 0, errors_ = 0;
    std::size_t history_ = 0, entities_ = 0;
    std::uint64_t revision_ = 0;
    ImVec2 pointer_{};
    std::string clipboard_, create_;
    bool clipboard_installed_ = false;
    const char* (*clipboard_get_)(ImGuiContext*) = nullptr;
    void* clipboard_user_ = nullptr;
};
}
}

#pragma once
#include <imgui.h>
#include <cstdint>
#include <string>
namespace dk::editor {
class Workspace;
namespace detail {
class WorkbenchUi;
class ConsoleContextSmoke final {
public:
    bool input(Workspace&,WorkbenchUi&);
private:
    unsigned tick_=0, errors_=0;
    std::size_t history_=0;
    std::uint64_t revision_=0;
    double action_time_=0;
    std::string first_entity_;
};
}
}

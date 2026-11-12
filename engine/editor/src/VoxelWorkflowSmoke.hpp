#pragma once
#include <filesystem>
#include <imgui.h>
namespace dk::editor { class Workspace; namespace detail {
class WorkbenchUi; class Viewport;
class VoxelWorkflowSmoke final {
public:
    explicit VoxelWorkflowSmoke(const std::filesystem::path& root);
    void input(WorkbenchUi&,Workspace&);
    void presented(WorkbenchUi&,Workspace&,Viewport&);
private:
    std::filesystem::path directory_;
    unsigned stage_=0,tick_=0;
    bool waiting_=false,ready_=false;
    ImVec2 pointer_{},start_{};
};
}}

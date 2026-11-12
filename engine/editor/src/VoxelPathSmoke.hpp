#pragma once
#include "WorkbenchUi.hpp"
namespace dk::editor::detail {
class VoxelPathSmoke final {
public:
    explicit VoxelPathSmoke(const std::filesystem::path& root);
    void input(WorkbenchUi&,Workspace&);
    void presented(WorkbenchUi&,Workspace&,Viewport&);
private:
    std::filesystem::path directory_;
    Json request_;
    std::string last_;
    unsigned tick_=0;
    ImVec2 pointer_{},start_{};
};
}

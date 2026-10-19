#pragma once
#include "WorkbenchUi.hpp"
#include <array>

namespace dk::editor::detail {
// Disposable acceptance fixture only. Fixed files synchronize with a separate dk-ctl process.
class ConsistencyDriver final {
public:
    explicit ConsistencyDriver(const std::filesystem::path& root,bool environment=false);
    void input(WorkbenchUi&,const Workspace&,const Viewport&);
    void presented(const Workspace&,const WorkbenchUi&,const Viewport&);
    [[nodiscard]] bool done() const { return done_; }
private:
    void verify_environment(const Workspace&,const WorkbenchUi&,const Viewport&) const;
    void verify(const Workspace&,const WorkbenchUi&,const Viewport&) const;
    std::filesystem::path directory_;
    std::size_t stage_ = 0;
    unsigned tick_ = 0;
    bool environment_ = false,clouds_=false;
    bool ready_ = false,waiting_ = false,done_ = false;
    ImVec2 pointer_{};
    std::array<Json,13> checkpoints_;
    std::uint64_t gizmo_revision_=0;
};
}

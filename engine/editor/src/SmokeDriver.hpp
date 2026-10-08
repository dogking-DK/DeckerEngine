#pragma once
#include "WorkbenchUi.hpp"
#include <dk/platform/Window.hpp>
namespace dk::editor::detail {
class SmokeDriver final {
public:
    explicit SmokeDriver(bool interaction=false) : interaction_(interaction) {}
    void input(Workspace&,WorkbenchUi&,platform::Window&,const Viewport&);
    [[nodiscard]] bool done() const { return done_; }
private:
    void interaction_input(Workspace&,WorkbenchUi&,platform::Window&,const Viewport&);
    void verify(Workspace&,WorkbenchUi&,const Viewport&,unsigned stage);
    ImVec2 pointer_{-10000,-10000};
    unsigned tick_ = 0;
    bool done_ = false;
    std::optional<EntityId> entity_;
    DocumentId session_;
    std::uint64_t revision_ = 0;
    std::string name_;
    double translation_ = 0;
    std::uint64_t pixels_ = 0;
    bool interaction_ = false;
    ImVec2 start_pointer_{};
    std::size_t history_=0;
    std::uint64_t stage_revision_=0,stage_pixels_=0,camera_pixels_=0;
    Trsd transformed_;
};
}

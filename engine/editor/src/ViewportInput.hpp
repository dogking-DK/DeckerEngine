#pragma once
#include "Viewport.hpp"
#include "CameraSettingsUi.hpp"
#include "NavigationUi.hpp"
#include <functional>
#include <map>

namespace dk::editor::detail {
class SettingsHelpUi;
class ViewportInput final {
public:
    using Report=std::function<bool(const char*,Result<void>)>;
    explicit ViewportInput(std::shared_ptr<EditorViewportService> host,RelativeMouse& mouse,bool fixture) : host_(std::move(host)),navigation_(mouse),fixture_(fixture) {}
    void toolbar(Workspace&,Viewport&,std::map<std::string,ImVec2>&,const Report&,SettingsHelpUi* = nullptr);
    void aspect(double value) { aspect_=value; host_->camera_aspect(value); }
    void image(Workspace&,Viewport&,std::map<std::string,ImVec2>&,const Report&);
    void cancel();
    void begin_frame() { navigation_.begin_frame(); }
    void end_frame() { navigation_.end_frame(); }
    [[nodiscard]] bool navigating() const { return navigation_.active(); }
    [[nodiscard]] bool camera_popup() const { return settings_ui_.pending(); }
    [[nodiscard]] bool active() const { return bool(edit_); }
    [[nodiscard]] Camera camera() const { return host_->camera(); }
    [[nodiscard]] const std::optional<TransformEdit>& edit() const { return edit_; }
    [[nodiscard]] std::uint64_t revision() const { return revision_; }
private:
    enum class Mode { translate,rotate,scale };
    std::shared_ptr<EditorViewportService> host_;
    void commit_camera(Workspace&,const Camera&,const Report&);
    void frame_camera(EntityId,const Report&);
    DocumentId camera_document_;
    CameraSettingsUi settings_ui_;
    NavigationUi navigation_;
    double aspect_=1;
    bool fixture_=false;
    Mode mode_=Mode::translate;
    std::optional<TransformEdit> edit_;
    std::uint64_t revision_=0;
    int axis_=0;
    Vec2d start_mouse_=Vec2d::Zero(),screen_axis_=Vec2d::Zero();
    Vec2d capture_minimum_=Vec2d::Zero(),capture_size_=Vec2d::Zero();
    Vec3d pivot_=Vec3d::Zero(),axis_world_=Vec3d::UnitX();
    Mat3d basis_=Mat3d::Identity();
    double units_=1,start_angle_=0;
};
}

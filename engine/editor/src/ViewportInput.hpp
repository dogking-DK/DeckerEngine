#pragma once
#include "Viewport.hpp"
#include <functional>
#include <map>

namespace dk::editor::detail {
class ViewportInput final {
public:
    using Report=std::function<bool(const char*,Result<void>)>;
    explicit ViewportInput(bool fixture) : camera_(fixture),fixture_(fixture) {}
    void toolbar(Workspace&,Viewport&,std::map<std::string,ImVec2>&);
    void image(Workspace&,Viewport&,std::map<std::string,ImVec2>&,const Report&);
    void cancel();
    [[nodiscard]] bool active() const { return bool(edit_); }
    [[nodiscard]] const Camera& camera() const { return camera_; }
    [[nodiscard]] const std::optional<TransformEdit>& edit() const { return edit_; }
    [[nodiscard]] std::uint64_t revision() const { return revision_; }
private:
    enum class Mode { translate,rotate,scale };
    Camera camera_;
    bool fixture_;
    Mode mode_=Mode::translate;
    std::optional<TransformEdit> edit_;
    std::uint64_t revision_=0;
    int axis_=0,navigation_=-1;
    Vec2d start_mouse_=Vec2d::Zero(),screen_axis_=Vec2d::Zero();
    Vec2d capture_minimum_=Vec2d::Zero(),capture_size_=Vec2d::Zero();
    Vec3d pivot_=Vec3d::Zero(),axis_world_=Vec3d::UnitX();
    Mat3d basis_=Mat3d::Identity();
    double units_=1,start_angle_=0;
};
}

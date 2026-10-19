#pragma once
#include <dk/geometry/Query.hpp>
#include <dk/math/Projection.hpp>
#include <cmath>

namespace dk::editor {
enum class NavigationMode { orbit, fly, walk };
enum class ProjectionMode { perspective, orthographic };
enum class StandardView { none, front, back, left, right, top, bottom };
struct CameraLensSettings {
    double fov_y=65;
    double ortho_height=2;
    bool automatic_clip=true;
    double near_plane=.05,far_plane=10000;
    bool operator==(const CameraLensSettings&) const = default;
};
struct CameraNavigationSettings {
    double move_speed=3,fast_multiplier=4,slow_multiplier=.25;
    double look_sensitivity=.003,orbit_sensitivity=.006,pan_multiplier=1,wheel_sensitivity=.15;
    bool invert_y=false;
    bool operator==(const CameraNavigationSettings&) const = default;
};
struct CameraSettings {
    CameraLensSettings lens;
    CameraNavigationSettings navigation;
    bool operator==(const CameraSettings&) const = default;
};
// Session-only camera. Settings and navigation never modify scene/history.
class Camera final {
public:
    explicit Camera(bool fixture=false) { reset(fixture); }
    void reset(bool fixture=false);
    // Context changes may make a manual lens unsupported; validate reports it without changing that lens.
    void set_environment(bool enabled);
    void set_pathtracing(bool enabled) { pathtracing_=enabled; }
    [[nodiscard]] bool orthographic_supported() const { return !environment_; }
    [[nodiscard]] ProjectionMode projection_mode() const { return projection_; }
    [[nodiscard]] StandardView standard_view() const { return standard_view_; }
    [[nodiscard]] Result<void> set_projection_mode(ProjectionMode,double aspect);
    [[nodiscard]] Result<void> set_standard_view(StandardView,double aspect);
    [[nodiscard]] Result<void> validate(double aspect) const;
    [[nodiscard]] Result<void> set_settings(const CameraSettings&,double aspect);
    [[nodiscard]] Result<void> restore_defaults(bool fixture,double aspect);
    [[nodiscard]] const CameraSettings& settings() const { return settings_; }
    [[nodiscard]] NavigationMode navigation_mode() const { return mode_; }
    [[nodiscard]] Result<void> set_navigation_mode(NavigationMode);
    [[nodiscard]] double fov_y() const { return settings_.lens.fov_y; }
    [[nodiscard]] double near_plane() const { return settings_.lens.automatic_clip ? (environment_ ? 1.0 : .05) : settings_.lens.near_plane; }
    [[nodiscard]] double far_plane() const { return settings_.lens.automatic_clip ? 10000 : settings_.lens.far_plane; }
    void orbit(double dx,double dy);
    void pan(double dx,double dy,double height);
    void dolly(double steps);
    [[nodiscard]] Result<void> frame(const geometry::Bounds&,double aspect);
    [[nodiscard]] Transformd world() const;
    [[nodiscard]] Mat4d projection(double aspect) const;
    [[nodiscard]] Mat4d clip(double aspect) const;
    [[nodiscard]] Result<geometry::Ray> ray(const Vec2d& uv,double aspect) const;
    [[nodiscard]] std::optional<Vec2d> project(const Vec3d&,double aspect) const;
    [[nodiscard]] double units_per_pixel(const Vec3d&,double height) const;
    [[nodiscard]] std::uint64_t revision() const { return revision_; }
private:
    friend class CameraNavigation;
    [[nodiscard]] PerspectiveLens lens() const { return {radians(fov_y()),near_plane(),far_plane()}; }
    [[nodiscard]] OrthographicLens ortho_lens() const { return {settings_.lens.ortho_height,near_plane(),far_plane()}; }
    [[nodiscard]] Result<Mat4d> projection_result(double aspect) const;
    [[nodiscard]] double tangent() const { return std::tan(radians(fov_y())/2); }
    [[nodiscard]] Vec3d backward() const;
    [[nodiscard]] Vec3d pivot() const { return eye_-backward()*distance_; }
    Vec3d eye_=Vec3d::Zero();
    double yaw_=0,pitch_=0,distance_=2;
    CameraSettings settings_;
    NavigationMode mode_=NavigationMode::orbit;
    NavigationMode perspective_mode_=NavigationMode::orbit;
    ProjectionMode projection_=ProjectionMode::perspective;
    StandardView standard_view_=StandardView::none;
    std::uint64_t revision_=0;
    bool environment_=false,pathtracing_=false;
};
}

#pragma once
#include <dk/geometry/Query.hpp>

namespace dk::editor {
// Session-only orbit camera. Projection uses Vulkan Y-down, depth [0,1].
class Camera final {
public:
    explicit Camera(bool fixture=false) { reset(fixture); }
    void reset(bool fixture=false);
    void orbit(double dx,double dy);
    void pan(double dx,double dy,double height);
    void dolly(double steps);
    void frame(const geometry::Bounds&,double aspect);
    [[nodiscard]] Transformd world() const;
    [[nodiscard]] Mat4d projection(double aspect) const;
    [[nodiscard]] Mat4d clip(double aspect) const;
    [[nodiscard]] Result<geometry::Ray> ray(const Vec2d& uv,double aspect) const;
    [[nodiscard]] std::optional<Vec2d> project(const Vec3d&,double aspect) const;
    [[nodiscard]] double units_per_pixel(const Vec3d&,double height) const;
    [[nodiscard]] std::uint64_t revision() const { return revision_; }
private:
    Vec3d target_=Vec3d::Zero();
    double yaw_=0,pitch_=0,distance_=2;
    std::uint64_t revision_=0;
};
}

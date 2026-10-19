#pragma once
#include <dk/math/Math.hpp>

namespace dk {
struct PerspectiveLens { double fov_y_radians,near_plane,far_plane; };
struct PerspectiveRay { Vec3d direction; double minimum,maximum; };
struct OrthographicLens { double height,near_plane,far_plane; };
struct OrthographicRay { Vec3d origin,direction; double minimum,maximum; };
// Right-handed camera looking along -Z, Y-down NDC and depth [0,1].
[[nodiscard]] Result<Mat4d> perspective_projection(const PerspectiveLens&,double aspect);
// UV is measured from the top-left. Distances terminate on the clipping planes.
[[nodiscard]] Result<PerspectiveRay> perspective_ray(const PerspectiveLens&,double aspect,const Vec2d& uv);
[[nodiscard]] Result<Mat4d> orthographic_projection(const OrthographicLens&,double aspect);
[[nodiscard]] Result<OrthographicRay> orthographic_ray(const OrthographicLens&,double aspect,const Vec2d& uv);
}

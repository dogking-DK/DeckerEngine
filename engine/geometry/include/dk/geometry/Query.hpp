#pragma once
#include <dk/math/Transform.hpp>
#include <limits>
#include <optional>
#include <span>
#include <vector>

namespace dk::geometry {
struct Ray {
    Vec3d origin = Vec3d::Zero(), direction = -Vec3d::UnitZ();
    double minimum = 0, maximum = std::numeric_limits<double>::max();
};
struct Bounds {
    Vec3d minimum = Vec3d::Constant(std::numeric_limits<double>::max());
    Vec3d maximum = Vec3d::Constant(-std::numeric_limits<double>::max());
    void include(const Vec3d& point) { minimum=minimum.cwiseMin(point); maximum=maximum.cwiseMax(point); }
    [[nodiscard]] bool empty() const { return (minimum.array()>maximum.array()).any(); }
};
struct Triangle { Vec3d a,b,c; };
struct Hit { double distance; std::size_t triangle; Vec3d barycentric; };
[[nodiscard]] Result<void> validate(const Ray&);
[[nodiscard]] Result<std::optional<double>> intersect(const Ray&,const Bounds&);
[[nodiscard]] Result<std::optional<Hit>> intersect(const Ray&,const Triangle&);
// Immutable, owning local-space acceleration data. Ray t is preserved under instance transforms.
class MeshQuery final {
public:
    [[nodiscard]] static Result<MeshQuery> build(std::span<const Triangle>);
    [[nodiscard]] Result<std::optional<Hit>> nearest(const Ray&) const;
    [[nodiscard]] Result<std::optional<Hit>> nearest(const Ray& world_ray,const Transformd& world) const;
    [[nodiscard]] const Bounds& bounds() const { return bounds_; }
private:
    struct Node { Bounds bounds; std::size_t first=0,count=0,left=0,right=0; };
    std::size_t split(std::size_t first,std::size_t count);
    void visit(std::size_t,const Ray&,std::optional<Hit>&) const;
    Bounds bounds_;
    std::vector<Triangle> triangles_;
    std::vector<std::size_t> order_;
    std::vector<Node> nodes_;
};
}

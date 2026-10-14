#pragma once
#include <dk/render/PathTraceReference.hpp>
#include <dk/memory/SmartPtr.hpp>
#include <numbers>

namespace dk::render::detail {
inline constexpr double path_pi = std::numbers::pi;
struct PathTriangle {
    Vec3d a, b, c, normal, albedo, emission;
    double area = 0, light_weight = 0, light_cdf = 0;
    bool double_sided = false;
    EntityId entity;
    AssetId mesh;
    std::size_t primitive = 0, triangle = 0;
};
struct PathSceneState {
    explicit PathSceneState(memory::ResourceHandle heap) : triangles(0, memory::Allocator<PathTriangle>{heap}) {}
    RenderScene source;
    Vector<PathTriangle> triangles;
    double light_weight = 0;
    std::size_t degenerate = 0;
    std::uint64_t fingerprint = 0;
};
struct PathImageState {
    explicit PathImageState(memory::ResourceHandle heap) : pixels(0, memory::Allocator<Vec3d>{heap}) {}
    PathTraceImageInfo info;
    Vector<Vec3d> pixels;
};
struct PathReferenceAccess {
    static const PathSceneState* get(const PathTraceScene& s) { return s.state_.get(); }
    static PathTraceImage image(std::shared_ptr<const PathImageState> s) { return PathTraceImage{std::move(s)}; }
};
inline Error path_invalid(std::string message) { return {ErrorCode::invalid_argument, std::move(message)}; }
inline Error path_cancelled() { return {ErrorCode::invalid_state, "CPU path tracing cancelled", {"render.pathtracing.reference.cancelled"}}; }
inline double luminance(const Vec3d& c) { return c.dot(Vec3d{.2126,.7152,.0722}); }
[[nodiscard]] Result<void> validate_path_ray(const PathTraceRay&);
[[nodiscard]] std::optional<PathTraceHit> nearest_path(const PathSceneState&, const PathTraceRay&, PathTraceStatistics&);
[[nodiscard]] Vec3d offset_path_origin(const PathTriangle&, const Vec3d& position, const Vec3d& direction);
} // namespace dk::render::detail

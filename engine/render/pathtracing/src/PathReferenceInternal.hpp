#pragma once
#include <dk/render/PathTraceReference.hpp>
#include <dk/render/VolumePathReference.hpp>
#include <dk/memory/SmartPtr.hpp>
#include <numbers>

namespace dk::render::detail {
inline constexpr double path_pi = std::numbers::pi;
struct PathTriangle {
    Vec3d a, b, c, normal, albedo, emission;
    double area = 0, light_weight = 0, light_cdf = 0;
    bool double_sided = false;
    Vec2d uv_a = Vec2d::Zero(), uv_b = Vec2d::Zero(), uv_c = Vec2d::Zero();
    std::uint32_t texture = 0xffffffffu;
    float alpha = 1, cutoff = .5f;
    float metallic = 0, roughness = 1;
    bool mask = false;
    EntityId entity;
    AssetId mesh;
    std::size_t primitive = 0, triangle = 0;
};
struct PathTexture {
    explicit PathTexture(memory::ResourceHandle heap) : rgba8(0,memory::Allocator<std::byte>{heap}),origin(memory::Allocator<char>{heap}) {}
    AssetId mesh, id;
    std::uint32_t width = 0, height = 0;
    TextureSampler sampler;
    Vector<std::byte> rgba8;
    String origin;
};
struct PathSceneState {
    explicit PathSceneState(memory::ResourceHandle heap) : owned_triangles(0, memory::Allocator<PathTriangle>{heap}),owned_textures(memory::Allocator<PathTexture>{heap}) {}
    RenderScene source;
    PathTraceMaterialModel material_model = PathTraceMaterialModel::lambert;
    Vector<PathTriangle> owned_triangles;
    Vector<PathTexture> owned_textures;
    std::span<const PathTriangle> triangles;
    std::span<const PathTexture> textures;
    std::shared_ptr<const PathSceneState> geometry_owner;
    double light_weight = 0;
    std::size_t degenerate = 0;
    std::size_t masked_emitters = 0;
    std::uint64_t fingerprint = 0;
    std::uint64_t geometry_fingerprint = 0;
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
[[nodiscard]] Result<void> validate_path_work(const PathSceneState&, std::uint64_t pixels, const PathTraceSettings&, PathTraceSamples, bool cpu_geometry=true);
[[nodiscard]] Result<void> validate_path_view(const PathSceneState&, const RenderView&, std::uint32_t maximum_extent = 64);
[[nodiscard]] Result<PathTraceRay> gpu_path_camera_ray(const RenderView&, std::uint32_t, std::uint32_t, const Vec2d&);
[[nodiscard]] std::optional<PathTraceHit> nearest_path(const PathSceneState&, const PathTraceRay&, PathTraceStatistics&);
[[nodiscard]] Vec3d offset_path_origin(const PathTriangle&, const Vec3d& position, const Vec3d& direction);
[[nodiscard]] Vec4d path_base_color(const PathSceneState&,const PathTriangle&,const Vec3d& barycentric);
[[nodiscard]] Result<void> validate_path_texture(const TextureData&);
[[nodiscard]] bool same_path_geometry(const PathSceneState&,const PathSceneState&);
[[nodiscard]] Result<void> validate_volume_work(const PathTraceScene&,const PathMediumSnapshot&,const VolumePathSettings&,
    PathTraceSamples,std::uint64_t pixels,bool cpu_geometry=true);
} // namespace dk::render::detail

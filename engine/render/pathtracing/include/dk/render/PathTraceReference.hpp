#pragma once
#include <dk/assets/CpuData.hpp>
#include <dk/render/RenderScene.hpp>
#include <limits>
#include <stop_token>

namespace dk::render {
namespace detail { struct PathSceneState; struct PathImageState; struct PathReferenceAccess; }

struct PathTraceRay {
    Vec3d origin = Vec3d::Zero(), direction = -Vec3d::UnitZ();
    double minimum = 0, maximum = std::numeric_limits<double>::max();
};
struct PathTraceHit {
    EntityId entity;
    AssetId mesh;
    std::size_t primitive = 0, triangle = 0, index = 0;
    double distance = 0;
    Vec3d barycentric = Vec3d::Zero(), position = Vec3d::Zero(), normal = Vec3d::Zero();
    bool front_face = true;
};
struct PathTraceSceneLimits {
    std::size_t max_triangles = 4096, max_primitive_instances = 4096;
    std::size_t max_textures = 64, max_texture_bytes = 64*1024*1024;
};
struct PathTraceTextureInfo {
    AssetId mesh, texture;
    std::string_view origin; // Borrowed from the immutable scene.
    std::uint32_t width = 0, height = 0;
};
enum class PathTraceMaterialModel { lambert, metallic_roughness };
// Immutable CPU input. Model v3 keeps Lambert and explicitly adds metallic-roughness.
class PathTraceScene final {
public:
    PathTraceScene() = default;
    [[nodiscard]] static Result<PathTraceScene> create(memory::ResourceHandle, const RenderScene&,
        std::span<const CpuAsset* const>, const PathTraceSceneLimits& = {}, PathTraceMaterialModel = PathTraceMaterialModel::lambert);
    [[nodiscard]] explicit operator bool() const noexcept { return bool(state_); }
    [[nodiscard]] SceneId id() const noexcept;
    [[nodiscard]] std::uint64_t revision() const noexcept;
    [[nodiscard]] std::uint64_t fingerprint() const noexcept;
    [[nodiscard]] std::size_t triangle_count() const noexcept;
    [[nodiscard]] std::size_t degenerate_count() const noexcept;
    [[nodiscard]] std::size_t texture_count() const noexcept;
    [[nodiscard]] std::size_t masked_emitter_count() const noexcept;
    [[nodiscard]] PathTraceMaterialModel material_model() const noexcept;
    [[nodiscard]] Result<PathTraceTextureInfo> texture_info(std::size_t) const;
    [[nodiscard]] Result<std::optional<PathTraceHit>> nearest(const PathTraceRay&) const;
private:
    friend struct detail::PathReferenceAccess;
    explicit PathTraceScene(std::shared_ptr<const detail::PathSceneState> state) : state_(std::move(state)) {}
    std::shared_ptr<const detail::PathSceneState> state_;
};
enum class PathTraceEstimator { mis, bsdf_only };
enum class PathTraceSun { none, scene };
struct PathTraceSettings {
    std::uint32_t model_version = 3, sampler_version = 1;
    std::uint64_t seed = 1;
    std::uint32_t max_bounces = 8, rr_start = 3; // 0 disables RR for comparison.
    Vec3d environment_rgb = Vec3d::Zero(); // Constant radiance; never includes a solar disk.
    PathTraceSun sun = PathTraceSun::none;
    PathTraceEstimator estimator = PathTraceEstimator::mis;
    bool surface_only = false; // Required acknowledgement when scene air/clouds are enabled.
    std::uint64_t max_rays = std::uint64_t{1} << 24;
    std::uint64_t max_triangle_tests = std::uint64_t{1} << 29;
};
struct PathTraceSamples { std::uint32_t first = 0, count = 256; };
struct PathTraceStatistics { std::uint64_t rays = 0, triangle_tests = 0; };
struct PathTraceEstimate {
    Vec3d mean = Vec3d::Zero(), second_moment = Vec3d::Zero();
    PathTraceStatistics statistics;
};
struct PathTraceImageInfo {
    std::uint32_t width = 0, height = 0;
    SceneId scene;
    std::uint64_t revision = 0, fingerprint = 0;
    std::size_t texture_count = 0, masked_emitter_count = 0;
    PathTraceMaterialModel material_model = PathTraceMaterialModel::lambert;
    ViewDescription view;
    PathTraceSettings settings;
    PathTraceSamples samples;
    PathTraceStatistics statistics;
};
class PathTraceImage final {
public:
    PathTraceImage() = default;
    [[nodiscard]] explicit operator bool() const noexcept { return bool(state_); }
    [[nodiscard]] std::span<const Vec3d> linear_rgb() const noexcept;
    [[nodiscard]] const PathTraceImageInfo& info() const noexcept;
private:
    friend struct detail::PathReferenceAccess;
    explicit PathTraceImage(std::shared_ptr<const detail::PathImageState> state) : state_(std::move(state)) {}
    std::shared_ptr<const detail::PathImageState> state_;
};
// Version 1: dimensions 0/1 pixel, 2 light class, 3 triangle, 4/5 light UV,
// 6/7 BSDF, 8 RR. Pure 32-bit counter hash, 24-bit open interval output.
[[nodiscard]] double path_trace_sample(std::uint64_t seed, std::uint32_t pixel,
    std::uint32_t sample, std::uint32_t bounce, std::uint32_t dimension) noexcept;
[[nodiscard]] Result<PathTraceRay> path_trace_camera_ray(const RenderView&, std::uint32_t x,
    std::uint32_t y, const Vec2d& subpixel);
[[nodiscard]] Result<PathTraceEstimate> estimate_path_reference(const PathTraceScene&, const PathTraceRay&,
    const PathTraceSettings& = {}, PathTraceSamples = {}, std::uint32_t pixel = 0, std::stop_token = {});
[[nodiscard]] Result<PathTraceImage> render_path_reference(memory::ResourceHandle, const PathTraceScene&,
    const RenderView&, const PathTraceSettings& = {}, PathTraceSamples = {}, std::stop_token = {});
} // namespace dk::render

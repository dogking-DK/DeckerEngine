#pragma once
#include <dk/render/PathTraceReference.hpp>
#include <dk/render/PathMediumReference.hpp>

namespace dk::render {
namespace detail { struct VolumeImageState; struct VolumeReferenceAccess; }
struct VolumePathSettings {
    std::uint32_t model_version=4, sampler_version=2;
    std::uint64_t seed=1;
    std::uint32_t max_bounces=8, rr_start=3; // Counts surface and real medium scatter; 0 disables RR.
    Vec3d environment_rgb=Vec3d::Zero(); // Distant boundary radiance, excludes the sun.
    bool sun=true;
    PathTraceEstimator estimator=PathTraceEstimator::mis;
    std::uint64_t max_rays=1ull<<24, max_triangle_tests=1ull<<29, max_tracking_steps=1ull<<26;
    std::uint32_t max_steps_per_path=1u<<16;
};
enum class VolumeSampleStream : std::uint32_t {
    channel, pixel, free_path, collision, phase_component, phase_direction,
    light_class, light_point, bsdf, roulette, shadow_path
};
// Sampler v2: stream/counter are distinct from physical event depth.
[[nodiscard]] double volume_path_sample(std::uint64_t seed,std::uint32_t pixel,std::uint32_t sample,
    std::uint32_t event,VolumeSampleStream stream,std::uint32_t counter=0) noexcept;
struct VolumePathStatistics {
    std::uint64_t rays=0, triangle_tests=0, tracking_steps=0, null_collisions=0;
    std::uint64_t medium_scatter=0, absorption=0, surface_scatter=0, ground_hits=0;
    std::uint64_t multiple_scatter_paths=0; // Paths with at least two real medium scatter events.
};
struct VolumePathEstimate {
    Vec3d mean=Vec3d::Zero(), second_moment=Vec3d::Zero();
    VolumePathStatistics statistics;
};
struct VolumePathImageInfo {
    SceneId scene;
    std::uint64_t revision=0, scene_fingerprint=0, field_hash=0;
    PathTraceMaterialModel material_model=PathTraceMaterialModel::lambert;
    ViewDescription view;
    EnvironmentDescription environment;
    PathMediumSettings medium;
    VolumePathSettings settings;
    PathTraceSamples samples;
    VolumePathStatistics statistics;
};
class VolumePathImage final {
public:
    VolumePathImage()=default;
    [[nodiscard]] explicit operator bool() const noexcept { return bool(state_); }
    [[nodiscard]] std::span<const Vec3d> linear_rgb() const noexcept;
    [[nodiscard]] std::span<const Vec3d> second_moments() const noexcept;
    [[nodiscard]] const VolumePathImageInfo& info() const noexcept;
private:
    friend struct detail::VolumeReferenceAccess;
    explicit VolumePathImage(std::shared_ptr<const detail::VolumeImageState> state) : state_(std::move(state)) {}
    std::shared_ptr<const detail::VolumeImageState> state_;
};
[[nodiscard]] Result<VolumePathEstimate> estimate_volume_path_reference(const PathTraceScene&,const PathMediumSnapshot&,
    const PathTraceRay&,const VolumePathSettings& = {},PathTraceSamples = {},std::uint32_t pixel=0,std::stop_token = {});
[[nodiscard]] Result<VolumePathImage> render_volume_path_reference(memory::ResourceHandle,const PathTraceScene&,
    const PathMediumSnapshot&,const RenderView&,const VolumePathSettings& = {},PathTraceSamples = {},std::stop_token = {});
} // namespace dk::render

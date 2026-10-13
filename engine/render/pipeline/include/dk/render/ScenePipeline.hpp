#pragma once
#include <dk/render/RenderScene.hpp>
#include <dk/render/GpuAssets.hpp>
#include <filesystem>
#include <array>

namespace dk::render {
class AtmosphereSky;
class CloudTextures;
struct CloudCompositionSettings;
struct CloudOptimizedSettings;
enum class CloudHistoryReset { none, new_view, disabled, resize, projection, camera, scene, content, quality, time, failure };
struct CloudFrameInfo {
    std::uint64_t view_id=0, source_sequence=0, candidate_sequence=0;
    std::uint32_t width=0,height=0; // Integration extent, not output extent.
    bool history_available=false;
    CloudHistoryReset reset=CloudHistoryReset::none;
};
struct CloudHistoryStatus { std::uint64_t completed=0,pending=0; bool reusable=false; };
struct CloudTraceStatistics {
    std::uint32_t primary_steps=0,cloud_lighting_steps=0,empty_steps=0,terminated_steps=0;
    std::uint32_t reconstructed_pixels=0,fallback_pixels=0,history_accepted=0,history_rejected=0;
    std::uint64_t density_samples=0;
};
namespace detail { struct PipelineState; struct FrameState; }
struct RenderSettings {
    Vec3f clear_rgb = Vec3f::Zero(); // Linear HDR color.
    float exposure = 1;
    bool capture_plan = false;
    bool sky_diffuse = true; // Used by lit paths; anchor-local sky irradiance.
    bool linear_readback = false; // Diagnostic: <= 65536 pixels, before exposure.
};
struct AtmosphereSettings {
    bool surface_lighting = true;
    bool sun_disk = true;
    bool reference = false; // Expensive direct integration; <= 4096 pixels.
};
// Shared by the editor viewport and Runtime capture for reproducible unlit previews.
[[nodiscard]] inline RenderSettings unlit_preview_settings() { return {{0.04f,0.08f,0.16f},1,false}; }
// Diagnostic source fingerprint; includes the shader include closure.
[[nodiscard]] Result<std::string> shader_fingerprint(const std::filesystem::path&, bool environment);
[[nodiscard]] Result<void> validate_render_settings(const RenderView&, const RenderSettings&);
struct FrameInfo {
    SceneId scene;
    std::uint64_t revision = 0, frame = 0;
    std::uint32_t width = 0, height = 0;
    std::size_t draw_count = 0;
};
// Shared immutable result metadata; GPU completion is explicit. Views borrow this owner.
class RenderFrame final {
public:
    RenderFrame() = default;
    [[nodiscard]] explicit operator bool() const noexcept { return bool(state_); }
    [[nodiscard]] FrameInfo info() const noexcept;
    [[nodiscard]] graphics::Submission submission() const noexcept;
    [[nodiscard]] Result<bool> wait(graphics::SubmissionQueue&, std::uint64_t timeout_ns = std::numeric_limits<std::uint64_t>::max()) const;
    [[nodiscard]] Result<void> read_rgba8(std::span<std::byte> destination) const;
    [[nodiscard]] Result<void> read_linear_rgba32f(std::span<std::array<float,4>> destination) const;
    // Only nonzero cloud frames with diagnostics enabled: cloud L.rgb / Tc.a,
    // or mixed T.rgb / extinction-weighted cloud distance in metres in alpha.
    [[nodiscard]] Result<void> read_cloud_rgba32f(std::span<std::array<float,4>> destination) const;
    [[nodiscard]] Result<void> read_medium_rgba32f(std::span<std::array<float,4>> destination) const;
    [[nodiscard]] CloudFrameInfo cloud_optimization() const noexcept;
    [[nodiscard]] Result<CloudTraceStatistics> read_cloud_statistics() const;
    [[nodiscard]] Result<const graphics::Image*> color() const;
    [[nodiscard]] std::string_view plan_text() const noexcept;
private:
    friend class ScenePipeline;
    explicit RenderFrame(std::shared_ptr<const detail::FrameState> state) : state_(std::move(state)) {}
    std::shared_ptr<const detail::FrameState> state_;
};
// Single queue, externally serialized. Does not own or store a raw queue pointer.
class ScenePipeline final {
public:
    ScenePipeline() = default;
    ScenePipeline(ScenePipeline&&) noexcept = default;
    ScenePipeline& operator=(ScenePipeline&&) noexcept = default;
    ScenePipeline(const ScenePipeline&) = delete;
    ScenePipeline& operator=(const ScenePipeline&) = delete;
    [[nodiscard]] static Result<ScenePipeline> create(memory::ResourceHandle, graphics::SubmissionQueue&,
        const std::filesystem::path& shader_directory);
    [[nodiscard]] Result<RenderFrame> render(graphics::SubmissionQueue&, const RenderView&, const GpuAssets&,
        const RenderSettings& = {});
    // Explicit Lambert path. Snapshot environment must match the RenderScene exactly.
    // A disabled scene uses unlit; requires DK_BUILD_RENDER_ATMOSPHERE when enabled.
    [[nodiscard]] Result<RenderFrame> render_sunlight(graphics::SubmissionQueue&, const RenderView&, const GpuAssets&,
        const AtmosphereSky&, const RenderSettings& = {});
    // Synchronous LUT preparation followed by asynchronous frame submission; call from a worker.
    [[nodiscard]] Result<RenderFrame> render_environment(graphics::SubmissionQueue&, const RenderView&, const GpuAssets&,
        const RenderSettings& = {});
    // Finite forward-Z perspective; the sky anchor must equal camera position.
    [[nodiscard]] Result<RenderFrame> render_atmosphere(graphics::SubmissionQueue&, const RenderView&, const GpuAssets&,
        const AtmosphereSky&, const RenderSettings& = {}, const AtmosphereSettings& = {});
    // Explicit full-sample composition, <=4096 pixels, no history. Sky and textures
    // must match the view's environment. Zero clouds use the original M14 path.
    [[nodiscard]] Result<RenderFrame> render_clouds(graphics::SubmissionQueue&, const RenderView&, const GpuAssets&,
        const AtmosphereSky&, const CloudTextures&, const CloudCompositionSettings&, const RenderSettings& = {});
    // Explicit half-resolution path, <=1920*1080 output pixels. Caller owns stable
    // view_id/camera_revision. Poll publishes completed candidates; at most four views.
    [[nodiscard]] Result<RenderFrame> render_clouds_optimized(graphics::SubmissionQueue&, const RenderView&, const GpuAssets&,
        const AtmosphereSky&, const CloudTextures&, const CloudOptimizedSettings&, const RenderSettings& = {});
    [[nodiscard]] Result<void> poll_cloud_history(graphics::SubmissionQueue&);
    [[nodiscard]] Result<CloudHistoryStatus> cloud_history(std::uint64_t view_id) const;
    [[nodiscard]] Result<void> release_cloud_history(graphics::SubmissionQueue&, std::uint64_t view_id);
private:
    [[nodiscard]] Result<RenderFrame> render_impl(graphics::SubmissionQueue&, const RenderView&, const GpuAssets&,
        const RenderSettings&, const AtmosphereSky*, const AtmosphereSettings* = nullptr,
        const CloudTextures* = nullptr, const CloudCompositionSettings* = nullptr, const CloudOptimizedSettings* = nullptr);
    explicit ScenePipeline(std::shared_ptr<detail::PipelineState> state) : state_(std::move(state)) {}
    std::shared_ptr<detail::PipelineState> state_;
};
} // namespace dk::render

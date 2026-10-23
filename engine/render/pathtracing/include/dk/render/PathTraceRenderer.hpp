#pragma once
#include <dk/render/PathTraceReference.hpp>
#include <dk/render/VolumePathReference.hpp>
#include <dk/render/RestirReference.hpp>
#include <dk/render/DenoisingSignals.hpp>
#include <dk/graphics/Resources.hpp>
#include <filesystem>

namespace dk::render {
namespace detail { struct PathGpuState; struct PathGpuScene; struct PathGpuFrame; struct PathGpuImage; struct PathGpuCompleted; }
// Immutable queue-domain scene; CPU source owns provenance and complete world geometry.
class PathTraceGpuScene final {
public:
    PathTraceGpuScene() = default;
    [[nodiscard]] explicit operator bool() const noexcept { return bool(state_); }
    [[nodiscard]] graphics::Submission submission() const noexcept; // Empty scene needs no BUILD submission.
    [[nodiscard]] const PathTraceScene& source() const noexcept;
private:
    friend class PathTraceRenderer;
    explicit PathTraceGpuScene(std::shared_ptr<const detail::PathGpuScene> state) : state_(std::move(state)) {}
    std::shared_ptr<const detail::PathGpuScene> state_;
};
struct PathTraceGpuMediumInfo {
    EnvironmentDescription environment;
    PathMediumSettings medium;
    VolumePathSettings settings; // max_tracking_steps is per dispatch (per tile in a progressive batch).
    std::uint64_t field_hash=0;
};
struct RestirTemporalStatistics {
    std::uint64_t reused=0, rejected=0, expired=0, support_corrected=0;
    std::uint32_t max_age=0, max_candidates=0;
};
struct RestirSpatialStatistics {
    std::uint64_t reused = 0, rejected = 0, support_corrected = 0, current_visibility_rays = 0;
    std::uint32_t max_candidates = 0, max_age = 0;
};
struct RestirMediumStatistics {
    std::uint64_t prepared_surfaces=0, applied_surfaces=0; // Complete progressive epochs only.
};
struct PathTraceGpuImageInfo : PathTraceImageInfo {
    std::optional<DenoisingSignalInfo> signals; // Complete progressive surface input only.
    std::uint64_t geometry_fingerprint=0;
    std::optional<PathTraceGpuMediumInfo> medium; // Absent for the legacy surface integrator.
    std::optional<RestirDiSettings> restir; // RTXDI 3.1.0; absent for classic paths.
    std::uint32_t sampling_first_epoch=0; // Random-stream origin of this image, distinct from spp.
    std::optional<std::uint32_t> temporal_history_epoch; // History at this image's start, if any.
    RestirTemporalStatistics temporal_statistics;
    RestirSpatialStatistics spatial_statistics;
    RestirMediumStatistics restir_medium_statistics;
    bool fixed_rays = false;
    std::uint32_t first_pixel = 0; // Counter origin for fixed-ray batches.
    // Hardware traversal counts are unavailable; statistics.triangle_tests stays zero.
};
class PathTraceGpuImage final {
public:
    PathTraceGpuImage() = default;
    [[nodiscard]] explicit operator bool() const noexcept { return bool(state_); }
    [[nodiscard]] std::span<const Vec3d> linear_rgb() const noexcept;
    [[nodiscard]] std::span<const Vec3d> second_moment() const noexcept;
    [[nodiscard]] std::span<const DenoisingSignalPixel> denoising_signals() const noexcept;
    // First sample's primary hit per pixel; indices refer to the prepared scene's CPU source.
    [[nodiscard]] std::span<const std::optional<PathTraceHit>> primary_hits() const noexcept;
    [[nodiscard]] const PathTraceGpuImageInfo& info() const noexcept;
private:
    friend class PathTraceFrame;
    explicit PathTraceGpuImage(std::shared_ptr<const detail::PathGpuImage> state) : state_(std::move(state)) {}
    std::shared_ptr<const detail::PathGpuImage> state_;
};
class PathTraceFrame final {
public:
    PathTraceFrame() = default;
    [[nodiscard]] explicit operator bool() const noexcept { return bool(state_); }
    [[nodiscard]] graphics::Submission submission() const noexcept;
    // No blocking wait. Empty optional means pending; errors/cancellation publish no partial image.
    [[nodiscard]] Result<std::optional<PathTraceGpuImage>> read(graphics::SubmissionQueue&, std::stop_token = {}) const;
private:
    friend class PathTraceRenderer;
    friend class PathTraceCompletedFrame;
    explicit PathTraceFrame(std::shared_ptr<const detail::PathGpuFrame> state) : state_(std::move(state)) {}
    std::shared_ptr<const detail::PathGpuFrame> state_;
};
struct PathTraceSchedule {
    std::uint32_t target_spp = 256, batch_spp = 1, tile_edge = 64; // Explicit tile edge: 1..256.
    std::uint64_t session = 0, gesture = 0;
    bool paused = false;
    double exposure = 0; // Display-only metadata; linear history is unchanged.
    bool reverse_tiles = false; // Traversal only; frozen at batch start, no sample identity change.
    std::uint32_t tiles_per_submission = 1; // 1..8, same pass only; further bounded by ray/candidate work.
    PathTraceSignalOptions signals; // Default Off. Toggling restarts raw and guides at a common sample zero.
};
struct PathTraceProgress {
    std::uint64_t view_id = 0, generation = 0;
    std::uint32_t completed_spp = 0, submitted_spp = 0, target_spp = 0;
    std::uint32_t completed_tiles = 0, total_tiles = 0;
    bool pending = false, paused = false, device_lost = false;
    double exposure = 0;
    // RIS: current private epoch and pass (0 surface/RIS, 3 temporal, 4 spatial, 1 shade).
    // completed_spp still advances only after the entire batch succeeds.
    std::uint32_t sampling_epoch = 0, sampling_pass = 0;
};
// Immutable completed GPU history. External holders remain charged to the renderer's budget.
class PathTraceCompletedFrame final {
public:
    PathTraceCompletedFrame() = default;
    [[nodiscard]] explicit operator bool() const noexcept { return bool(state_); }
    [[nodiscard]] const PathTraceGpuImageInfo& info() const noexcept;
    // Eight float4 per pixel: mean, moment, hit, normal, sum/count, correction, moment sum/correction.
    [[nodiscard]] const graphics::Buffer* linear_buffer() const noexcept;
    [[nodiscard]] const graphics::Buffer* signal_buffer() const noexcept; // 21 float4/pixel, or nullptr when Off.
    [[nodiscard]] std::optional<DenoisingMotionReference> signal_reference() const;
    [[nodiscard]] Result<PathTraceFrame> read_linear(graphics::SubmissionQueue&, std::stop_token = {}) const;
private:
    friend class PathTraceRenderer;
    explicit PathTraceCompletedFrame(std::shared_ptr<const detail::PathGpuCompleted> s) : state_(std::move(s)) {}
    std::shared_ptr<const detail::PathGpuCompleted> state_;
};
struct PathTraceDisplayFrame {
    graphics::Image image;
    graphics::Submission submission;
};
struct PathTraceMemoryStats {
    graphics::AllocationBudgetStats owned;
    std::uint64_t device_allocation_bytes = 0, device_block_bytes = 0;
};
// One queue, externally serialized. Two view slots, one progressive batch; no implicit waits.
class PathTraceRenderer final {
public:
    static constexpr std::uint32_t restir_max_candidates_per_invocation = 1u << 18;
    static constexpr std::uint64_t max_memory_limit = std::numeric_limits<std::uint64_t>::max();
    // Current device-local budget minus existing use; zero default selects this policy.
    [[nodiscard]] static std::uint64_t recommended_memory_limit(const graphics::Device&);
    PathTraceRenderer() = default;
    [[nodiscard]] static Result<PathTraceRenderer> create(memory::ResourceHandle, graphics::SubmissionQueue&,
        const std::filesystem::path& shader_directory, std::stop_token = {},
        std::uint64_t memory_limit = 0);
    [[nodiscard]] Result<PathTraceGpuScene> prepare_scene(graphics::SubmissionQueue&, const PathTraceScene&, std::stop_token = {}) const;
    [[nodiscard]] Result<PathTraceFrame> submit_view(graphics::SubmissionQueue&, const PathTraceGpuScene&, const RenderView&,
        const PathTraceSettings& = {}, PathTraceSamples = {}, std::stop_token = {}) const;
    [[nodiscard]] Result<PathTraceFrame> submit_rays(graphics::SubmissionQueue&, const PathTraceGpuScene&, std::span<const PathTraceRay>,
        const PathTraceSettings& = {}, PathTraceSamples = {}, std::uint32_t first_pixel = 0, std::stop_token = {}) const;
    [[nodiscard]] Result<void> configure_view(graphics::SubmissionQueue&, std::uint64_t view_id,
        const PathTraceGpuScene&, const RenderView&, const PathTraceSettings& = {}, const PathTraceSchedule& = {});
    // No history: split high spp into consecutive sample intervals when count*candidates > 2^18.
    [[nodiscard]] Result<PathTraceFrame> submit_restir_rays(graphics::SubmissionQueue&, const PathTraceGpuScene&,
        std::span<const PathTraceRay>, const PathTraceSettings& = {}, const RestirDiSettings& = {},
        PathTraceSamples = {}, std::uint32_t first_pixel = 0, std::stop_token = {}) const;
    [[nodiscard]] Result<PathTraceFrame> submit_restir_view(graphics::SubmissionQueue&, const PathTraceGpuScene&,
        const RenderView&, const PathTraceSettings& = {}, const RestirDiSettings& = {}, PathTraceSamples = {}, std::stop_token = {}) const;
    [[nodiscard]] Result<void> configure_restir_view(graphics::SubmissionQueue&, std::uint64_t view_id,
        const PathTraceGpuScene&, const RenderView&, const PathTraceSettings& = {}, const RestirDiSettings& = {}, const PathTraceSchedule& = {});
    // Model 4 / sampler 2. Requires a matching frozen environment with clouds disabled.
    [[nodiscard]] Result<PathTraceFrame> submit_air_rays(graphics::SubmissionQueue&,const PathTraceGpuScene&,
        const PathMediumSnapshot&,std::span<const PathTraceRay>,const VolumePathSettings& = {},PathTraceSamples = {},
        std::uint32_t first_pixel=0,std::stop_token = {}) const;
    [[nodiscard]] Result<PathTraceFrame> submit_air_view(graphics::SubmissionQueue&,const PathTraceGpuScene&,
        const PathMediumSnapshot&,const RenderView&,const VolumePathSettings& = {},PathTraceSamples = {},std::stop_token = {}) const;
    [[nodiscard]] Result<void> configure_air_view(graphics::SubmissionQueue&,std::uint64_t view_id,const PathTraceGpuScene&,
        const PathMediumSnapshot&,const RenderView&,const VolumePathSettings& = {},const PathTraceSchedule& = {});
    // Model 4 / sampler 2, including nonuniform clouds; same frozen input and history rules.
    [[nodiscard]] Result<PathTraceFrame> submit_medium_rays(graphics::SubmissionQueue&,const PathTraceGpuScene&,
        const PathMediumSnapshot&,std::span<const PathTraceRay>,const VolumePathSettings& = {},PathTraceSamples = {},
        std::uint32_t first_pixel=0,std::stop_token = {}) const;
    [[nodiscard]] Result<PathTraceFrame> submit_medium_view(graphics::SubmissionQueue&,const PathTraceGpuScene&,
        const PathMediumSnapshot&,const RenderView&,const VolumePathSettings& = {},PathTraceSamples = {},std::stop_token = {}) const;
    [[nodiscard]] Result<void> configure_medium_view(graphics::SubmissionQueue&,std::uint64_t view_id,const PathTraceGpuScene&,
        const PathMediumSnapshot&,const RenderView&,const VolumePathSettings& = {},const PathTraceSchedule& = {});
    // Model 4/sampler 2. Full RGB reservoirs are independent of hero/free-flight survival.
    // Fixed work is initial RIS only; progressive views optionally reuse time/space.
    [[nodiscard]] Result<PathTraceFrame> submit_restir_medium_rays(graphics::SubmissionQueue&,const PathTraceGpuScene&,
        const PathMediumSnapshot&,std::span<const PathTraceRay>,const VolumePathSettings& = {},const RestirDiSettings& = {},
        PathTraceSamples = {},std::uint32_t first_pixel=0,std::stop_token = {}) const;
    [[nodiscard]] Result<PathTraceFrame> submit_restir_medium_view(graphics::SubmissionQueue&,const PathTraceGpuScene&,
        const PathMediumSnapshot&,const RenderView&,const VolumePathSettings& = {},const RestirDiSettings& = {},
        PathTraceSamples = {},std::stop_token = {}) const;
    [[nodiscard]] Result<void> configure_restir_medium_view(graphics::SubmissionQueue&,std::uint64_t view_id,const PathTraceGpuScene&,
        const PathMediumSnapshot&,const RenderView&,const VolumePathSettings& = {},const RestirDiSettings& = {},const PathTraceSchedule& = {});
    // Submits one bounded group of tiles. Repeated calls while pending are no-ops.
    [[nodiscard]] Result<bool> submit_batch(graphics::SubmissionQueue&, std::uint64_t view_id, std::stop_token = {});
    // Collects one group without submitting more work. True only when a whole batch is published.
    [[nodiscard]] Result<bool> poll(graphics::SubmissionQueue&, std::stop_token = {});
    [[nodiscard]] Result<void> reset(std::uint64_t view_id);
    [[nodiscard]] Result<void> cancel(std::uint64_t view_id); // Retains completed history, discards current batch.
    [[nodiscard]] Result<void> release_view(std::uint64_t view_id);
    [[nodiscard]] Result<PathTraceProgress> status(std::uint64_t view_id) const;
    // Current tile ticket for explicit wait/profiling. Empty between tiles; no state change.
    [[nodiscard]] graphics::Submission pending_submission() const noexcept;
    [[nodiscard]] PathTraceCompletedFrame completed_frame(std::uint64_t view_id) const noexcept;
    // GPU-only Reinhard display. Exposure is in EV; encode_srgb only for a non-sRGB destination.
    [[nodiscard]] Result<PathTraceDisplayFrame> display(graphics::SubmissionQueue&, const PathTraceCompletedFrame&,
        double exposure = 0, bool encode_srgb = false, std::stop_token = {}) const;
    [[nodiscard]] PathTraceMemoryStats memory_stats(const graphics::SubmissionQueue&) const noexcept;

private:
    explicit PathTraceRenderer(std::shared_ptr<detail::PathGpuState> state) : state_(std::move(state)) {}
    std::shared_ptr<detail::PathGpuState> state_;
};
} // namespace dk::render

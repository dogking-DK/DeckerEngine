#pragma once
#include <dk/services/SceneService.hpp>
#include <dk/jobs/JobQueue.hpp>
#include <dk/render/ScenePipeline.hpp>
#include <dk/assets/GltfImporter.hpp>

namespace dk {
struct CaptureCamera {
    Vec3d eye{8,1.8,0}, target{-4,2,0}, up{0,1,0};
    double fov_y=65, near_plane=0.05, far_plane=100;
    bool orthographic=false, explicit_fov_y=false;
    std::optional<double> ortho_height;
};
struct CaptureSample {
    CaptureCamera camera;
    double time_seconds=0;
    bool camera_cut=false;
};
// Command values only; no renderer or third-party SDK ABI in the service interface.
struct CaptureRestirTemporal {
    std::uint32_t version=1,max_history_candidates=64,max_history_age=8;
    double position_tolerance=.1,normal_cosine=.95;
};
struct CaptureRestirSpatial {
    std::uint32_t version=1,neighbor_count=4,radius=2,max_source_candidates=64;
    double position_tolerance=1,normal_cosine=.9;
};
struct CaptureRestir {
    std::uint32_t version=1,initial_candidates=8;
    std::uint64_t max_candidate_evaluations=1ull<<24;
    std::optional<CaptureRestirTemporal> temporal;
    std::optional<CaptureRestirSpatial> spatial;
};
struct CapturePathTrace {
    std::uint32_t version=1,target_spp=256,max_bounces=8,rr_start=3;
    std::uint64_t seed=1;
    Vec3d environment_rgb=Vec3d::Zero();
    bool sun=false,surface_only=false;
    double exposure_ev=0;
    bool scene_media=false; // v2 explicitly chooses a transport mode.
    double time_seconds=0,cloud_albedo=.9,cloud_anisotropy=.6;
    std::uint64_t max_tracking_steps=1ull<<26;
    std::uint32_t max_steps_per_path=1u<<16;
    bool restir_di=false; // v3; classic MIS remains the default for old clients.
    std::optional<CaptureRestir> restir;
};
struct CaptureRequest {
    std::string output;
    std::uint32_t width=960, height=540;
    CaptureCamera camera;
    GltfImportProfile profile=GltfImportProfile::unlit_preview;
    bool require_validation=false;
    float exposure=1;
    render::EnvironmentRenderSettings environment;
    std::uint32_t warmup_frames=0;
    std::vector<CaptureSample> samples; // Empty: one PPM. Otherwise: atomic sequence JSON.
    bool pathtrace=false,linear_json=false;
    std::optional<CapturePathTrace> path;
};
struct CaptureInfo {
    DocumentId document;
    render::FrameInfo frame;
    std::string output;
    std::string provenance; // Frozen JSON; source fingerprint is completed by the worker.
};
struct CaptureTicket { JobId job; CaptureInfo info; };
// Owner-thread facade; worker owns all GPU objects and never accesses the SceneService.
class CaptureService final {
public:
    [[nodiscard]] static Result<std::unique_ptr<CaptureService>> create(
        std::filesystem::path shader_directory, std::function<void()> wake = {});
    ~CaptureService();
    [[nodiscard]] Result<CaptureTicket> capture(const SceneService&,EditGuard,const CaptureRequest&);
    [[nodiscard]] Result<JobSnapshot> job(JobId) const;
    [[nodiscard]] Result<JobCancel> cancel(JobId);
    [[nodiscard]] JobLimits limits() const noexcept;
    void pump();
    void rethrow_failure() const;
    void close() noexcept;
private:
    struct Impl;
    explicit CaptureService(std::unique_ptr<Impl>);
    std::unique_ptr<Impl> impl_;
};
}

#pragma once
#include <dk/services/SceneService.hpp>
#include <dk/jobs/JobQueue.hpp>
#include <dk/render/ScenePipeline.hpp>
#include <dk/assets/GltfImporter.hpp>

namespace dk {
struct CaptureCamera {
    Vec3d eye{8,1.8,0}, target{-4,2,0}, up{0,1,0};
    double fov_y=65, near_plane=0.05, far_plane=100;
};
struct CaptureSample {
    CaptureCamera camera;
    double time_seconds=0;
    bool camera_cut=false;
};
struct CapturePathTrace {
    std::uint32_t version=1,target_spp=256,max_bounces=8,rr_start=3;
    std::uint64_t seed=1;
    Vec3d environment_rgb=Vec3d::Zero();
    bool sun=false,surface_only=false;
    double exposure_ev=0;
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

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
struct CaptureRequest {
    std::string output;
    std::uint32_t width=960, height=540;
    CaptureCamera camera;
    GltfImportProfile profile=GltfImportProfile::unlit_preview;
    bool require_validation=false;
};
struct CaptureInfo {
    DocumentId document;
    render::FrameInfo frame;
    std::string output;
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

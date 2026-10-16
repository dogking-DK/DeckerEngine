#pragma once
#include <dk/editor/Workspace.hpp>
#include <dk/editor/Camera.hpp>
#include <dk/render/DiskScene.hpp>
#include <dk/render/ScenePipeline.hpp>
#include <dk/graphics/CommandEncoder.hpp>
#include <imgui.h>
#include <chrono>
#include "EnvironmentPreview.hpp"
#ifdef DK_EDITOR_PATHTRACING
#include "PathPreview.hpp"
#endif

namespace dk::editor::detail {
class Viewport final {
public:
    Viewport(memory::MemorySystem&,memory::ResourceHandle heap,graphics::SubmissionQueue& queue,const graphics::DeviceOptions&,bool retain_pixels=false);
    ~Viewport();
    // Previous GUI submission must be complete. Backend must be initialized.
    void update(const SceneReadSnapshot&,std::uint32_t width,std::uint32_t height,bool srgb,const Camera&,
        const std::optional<TransformEdit>&,std::uint64_t preview_revision,float exposure=1,const render::EnvironmentRenderSettings& environment = {});
    void update_simulation(Workspace&,const SimulationRunState&,std::uint32_t width,std::uint32_t height,const Camera&);
    void show_edit();
    void suspend_path();
    void close_path();
    [[nodiscard]] const std::string& path_unavailable() const { return path_unavailable_; }
#ifdef DK_EDITOR_PATHTRACING
    void update_path(const SceneReadSnapshot&,std::uint32_t,std::uint32_t,bool,const Camera&,const std::optional<TransformEdit>&,std::uint64_t,const PathPreviewSettings&);
    [[nodiscard]] PathPreviewStatus path_status() const { return path_ ? path_->status() : PathPreviewStatus{}; }
    [[nodiscard]] const render::PathTraceGpuImageInfo& path_info() const { return path_info_; }
    [[nodiscard]] std::uint32_t path_samples() const { return path_samples_; }
    [[nodiscard]] bool path_interactive() const { return path_interactive_; }
    // Acceptance calls this after the GUI submission has completed.
    [[nodiscard]] double path_latency_ms() const {
        return std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-path_published_request_).count();
    }
    std::uint64_t capture_path_pixels();
    [[nodiscard]] std::span<const Vec3d> path_linear() const { return path_linear_.linear_rgb(); }
#endif
    void retry_simulation() { simulation_view_.reset(); }
    [[nodiscard]] std::optional<SimulationId> simulation_run() const { return simulation_run_; }
    [[nodiscard]] std::uint64_t simulation_steps() const { return simulation_steps_; }
    [[nodiscard]] std::uint64_t simulation_view_sequence() const { return simulation_published_; }
    [[nodiscard]] bool simulation_view_current() const { return simulation_published_==simulation_sequence_; }
    // Explicit acceptance-only observation; the regular viewport never reads back a simulation image.
    std::uint64_t capture_simulation_pixels();
    [[nodiscard]] Result<std::optional<EntityId>> pick(const Vec2d& uv) const;
    [[nodiscard]] geometry::Bounds bounds(EntityId,const SceneSnapshot&) const;
    [[nodiscard]] bool camera_current(const Camera& camera) const { return !simulation_run_ && published_camera_==camera.revision(); }
    void retry() { attempted_.reset(); asset_session_.reset(); ++path_reload_; }
    [[nodiscard]] std::uint64_t environment_discarded() const { return environment_.discarded(); }
    [[nodiscard]] const render::EnvironmentRenderSettings& environment_settings() const { return published_environment_; }
    [[nodiscard]] const render::CloudFrameInfo& environment_history() const { return published_history_; }
    [[nodiscard]] std::uint32_t environment_warmup() const { return published_warmup_; }
    [[nodiscard]] std::uint64_t environment_shadow() const { return published_shadow_; }
    void release_texture();
    [[nodiscard]] ImTextureID texture() const { return reinterpret_cast<ImTextureID>(descriptor_); }
    [[nodiscard]] const graphics::Image& image() const { return image_; }
    [[nodiscard]] const graphics::ImageView& image_view() const { return view_; }
    [[nodiscard]] std::uint64_t pixel_signature() const { return pixel_signature_; }
    [[nodiscard]] const render::FrameInfo& info() const { return info_; }
    // Camera of the last Edit image, used for entity picking and Edit consistency checks only.
    [[nodiscard]] const Camera& published_camera() const { return camera_; }
    [[nodiscard]] std::span<const std::byte> retained_pixels() const { return pixels_; }
    [[nodiscard]] bool current(const DocumentState& s) const { return published_session_==s.document_id && info_.revision==s.revision && published_==attempted_; }
    [[nodiscard]] const std::string& error() const { return error_; }
private:
    struct Key {
        DocumentId session; std::uint64_t revision; std::uint32_t width,height; bool srgb;
        std::uint64_t camera,preview;
        float exposure;
        render::EnvironmentRenderSettings environment;
        bool operator==(const Key&) const = default;
    };
    void publish(std::vector<std::byte>,const render::FrameInfo&,render::RenderScene,const Camera&,const Key&);
    std::string path_unavailable_="Path tracing is not compiled in this editor.";
    std::uint64_t path_reload_=0;
#ifdef DK_EDITOR_PATHTRACING
    std::unique_ptr<PathPreview> path_;
    std::optional<PathPreviewSettings> path_settings_;
    std::uint64_t path_sequence_=0;
    std::uint32_t path_samples_=0;
    render::PathTraceGpuImage path_linear_;
    render::PathTraceGpuImageInfo path_info_;
    bool path_active_=false;
    bool path_interactive_=false;
    std::uint64_t path_observed_camera_=0;
    std::chrono::steady_clock::time_point path_motion_until_{},path_requested_at_{},path_published_request_{};
#endif
    memory::ResourceHandle heap_;
    graphics::SubmissionQueue& queue_;
    render::ScenePipeline pipeline_;
    EnvironmentPreview environment_;
    std::uint64_t environment_sequence_=0,published_shadow_=0;
    render::EnvironmentRenderSettings published_environment_;
    render::CloudFrameInfo published_history_;
    std::uint32_t published_warmup_=0;
    std::optional<DocumentId> asset_session_,published_session_;
    std::optional<Key> attempted_,published_;
    std::optional<Key> environment_key_;
    Camera environment_camera_;
    bool environment_has_frame_=true;
    std::optional<SimulationId> simulation_run_;
    std::optional<SimulationView> simulation_view_;
    std::uint64_t simulation_steps_=0,simulation_sequence_=0,simulation_published_=0;
    render::GpuAssets assets_;
    graphics::Image image_;
    graphics::ImageView view_;
    VkDescriptorSet descriptor_ = VK_NULL_HANDLE;
    render::FrameInfo info_{};
    std::string error_;
    std::uint64_t pixel_signature_ = 0;
    std::uint64_t published_camera_ = 0;
    bool retain_pixels_ = false;
    std::vector<std::byte> pixels_;
    Camera camera_;
    render::RenderScene scene_;
    std::vector<PickMesh> meshes_;
};
}

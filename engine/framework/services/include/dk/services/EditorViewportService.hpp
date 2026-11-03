#pragma once
#include <dk/services/SceneService.hpp>
#include <functional>
#include <memory>

namespace dk {
struct EditorViewIdTag;
using EditorViewId = StableId<EditorViewIdTag>;
// CPU values shared by the host service and render worker; no graphics/UI dependency.
struct ViewportPathSettings {
    std::uint32_t target=64,seed=1,bounces=4;
    Vec3d sky{.3,.4,.6};
    double exposure=0;
    bool paused=false,sun=true,surface_only=true,scene_media=false,sky_enabled=true;
    double time_seconds=0,cloud_albedo=.9,cloud_anisotropy=.6;
    bool restir_di=false,restir_temporal=false,restir_spatial=false;
    std::uint32_t restir_candidates=8;
    std::uint64_t reset=0;
    bool operator==(const ViewportPathSettings&) const = default;
};
struct EditorViewportSettings {
    bool path_trace=false,unlit_inspection=false;
    double raster_exposure=1;
    ViewportPathSettings path;
    bool operator==(const EditorViewportSettings&) const = default;
};
struct EditorViewportGuard {
    EditorViewId view_id;
    DocumentId document_id;
    std::uint64_t revision=0;
    bool operator==(const EditorViewportGuard&) const = default;
};
struct EditorViewportContext { DocumentId document_id; std::uint64_t scene_revision=0; std::string blocked_reason; };
struct EditorViewportCapabilities { bool pathtracing=false; std::string unavailable; };
struct EditorViewportPresentation {
    EditorViewportGuard observed;
    std::optional<EditorViewportGuard> shown;
    DocumentId document_id;
    std::uint64_t scene_revision=0,camera_revision=0,generation=0;
    std::uint32_t width=0,height=0,spp=0,target_spp=0;
    bool has_image=false,current=false;
    std::string renderer="raster",phase="Idle",error;
};
struct EditorViewportState {
    EditorViewportGuard guard;
    EditorViewportSettings settings;
    EditorViewportCapabilities capabilities;
    std::string blocked_reason;
    EditorViewportPresentation presentation;
};
enum class EditorViewportControl { pause,resume,reset };
class EditorViewportService final {
public:
    // Owner-thread only. The host must close before destroying the context owner.
    using Context = std::function<Result<EditorViewportContext>()>;
    static Result<std::shared_ptr<EditorViewportService>> create(EditorViewportCapabilities,Context);
    Result<EditorViewportState> query() const;
    Result<EditorViewportState> set(EditorViewportGuard,const EditorViewportSettings&,bool explicit_path_fields=false);
    Result<EditorViewportState> control(EditorViewportGuard,EditorViewportControl);
    const EditorViewportSettings& settings() const { return settings_; }
    void present(EditorViewportGuard,EditorViewportPresentation);
    void close();
private:
    EditorViewportService(EditorViewId id,EditorViewportCapabilities capabilities,Context context)
        : id_(id),capabilities_(std::move(capabilities)),context_(std::move(context)) {}
    Result<EditorViewportState> write(EditorViewportGuard,const EditorViewportSettings&,bool explicit_path,bool resetting);
    EditorViewId id_;
    std::uint64_t revision_=0;
    EditorViewportCapabilities capabilities_;
    Context context_;
    EditorViewportSettings settings_;
    EditorViewportPresentation presentation_;
    bool closed_=false;
};
}

#pragma once
#include "EnvironmentPreview.hpp"
#include <dk/render/PathTraceRenderer.hpp>
#include <dk/graphics/Transfer.hpp>
#include <dk/services/EditorViewportService.hpp>
namespace dk::editor::detail {
using PathPreviewSettings = ViewportPathSettings;
struct PathPreviewRequest {
    SceneReadSnapshot snapshot;
    Camera camera;
    std::uint32_t width=0,height=0;
    bool srgb=true;
    std::optional<TransformEdit> edit;
    std::uint64_t preview=0,reload=0,sequence=0;
    PathPreviewSettings settings;
    bool read_linear=false; // Explicit acceptance-only readback, never normal presentation.
    bool observe_memory=false; // Explicit benchmark-only VMA snapshot.
};
struct PathPreviewStatus {
    std::uint64_t sequence=0;
    std::uint64_t gpu_memory_limit=0;
    render::PathTraceProgress progress;
    std::string phase="Idle",error,reset_reason="No completed configuration";
};
struct PathPreviewResult {
    std::uint64_t sequence=0;
    DocumentState document;
    Camera camera;
    render::PathTraceGpuImageInfo info;
    render::RenderScene scene;
    std::vector<PickMesh> meshes;
    graphics::ImageTransfer image;
    render::PathTraceGpuImage linear;
    render::PathTraceMemoryStats memory;
};
// One replaceable input/completion. The worker owns all rendering and asset preparation.
class PathPreview final {
public:
    PathPreview(memory::MemorySystem&,memory::ResourceHandle,graphics::Device);
    ~PathPreview();
    void request(PathPreviewRequest);
    void suspend();
    std::optional<PathPreviewResult> take_result();
    PathPreviewStatus status() const;
private:
    void run(std::stop_token);
    void phase(std::uint64_t,std::string,const render::PathTraceProgress& = {});
    memory::MemorySystem& memory_;
    memory::ResourceHandle heap_;
    graphics::Device device_;
    mutable std::mutex mutex_;
    std::condition_variable_any condition_;
    std::optional<PathPreviewRequest> request_;
    std::optional<PathPreviewResult> result_;
    PathPreviewStatus status_;
    std::stop_source cancel_;
    bool enabled_=false;
    std::uint64_t wanted_=0;
    std::jthread thread_;
};
}

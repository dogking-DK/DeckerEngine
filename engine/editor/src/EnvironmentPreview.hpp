#pragma once
#include <dk/editor/Workspace.hpp>
#include <dk/editor/Camera.hpp>
#include <dk/render/ScenePipeline.hpp>
#include <dk/memory/MemorySystem.hpp>
#include <condition_variable>
#include <mutex>
#include <thread>
namespace dk::editor::detail {
struct PickMesh { AssetId id; geometry::MeshQuery query; };
struct EnvironmentPreviewRequest {
    SceneReadSnapshot snapshot;
    Camera camera;
    std::uint32_t width,height;
    std::optional<TransformEdit> edit;
    std::uint64_t sequence;
    float exposure;
    render::EnvironmentRenderSettings environment;
};
struct EnvironmentPreviewResult {
    std::uint64_t sequence=0;
    render::FrameInfo info;
    render::RenderScene scene;
    std::vector<PickMesh> meshes;
    std::vector<std::byte> pixels;
    std::string error;
    render::EnvironmentRenderSettings environment;
    render::CloudFrameInfo history;
    std::uint64_t shadow_generation=0;
    std::uint32_t warmup_frames=0;
};
// One active, one replaceable request, one replaceable completion. No GUI state on worker.
class EnvironmentPreview final {
public:
    EnvironmentPreview(memory::MemorySystem&,memory::ResourceHandle,const graphics::DeviceOptions&);
    ~EnvironmentPreview();
    void request(EnvironmentPreviewRequest);
    void cancel();
    std::optional<EnvironmentPreviewResult> take_result();
    std::uint64_t discarded() const;
    std::uint64_t active_sequence() const;
private:
    void run(std::stop_token);
    bool current(std::uint64_t) const;
    memory::MemorySystem& memory_;
    memory::ResourceHandle heap_;
    graphics::DeviceOptions options_;
    mutable std::mutex mutex_;
    std::condition_variable_any condition_;
    std::optional<EnvironmentPreviewRequest> request_;
    std::optional<EnvironmentPreviewResult> result_;
    std::uint64_t wanted_=0,discarded_=0,active_=0;
    std::jthread thread_;
};
}

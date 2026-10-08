#pragma once
#include <dk/editor/Workspace.hpp>
#include <dk/editor/Camera.hpp>
#include <dk/render/DiskScene.hpp>
#include <dk/render/ScenePipeline.hpp>
#include <dk/graphics/CommandEncoder.hpp>
#include <imgui.h>

namespace dk::editor::detail {
class Viewport final {
public:
    Viewport(memory::ResourceHandle heap,graphics::SubmissionQueue& queue);
    ~Viewport();
    // Previous GUI submission must be complete. Backend must be initialized.
    void update(const SceneReadSnapshot&,std::uint32_t width,std::uint32_t height,bool srgb,const Camera&,
        const std::optional<TransformEdit>&,std::uint64_t preview_revision);
    [[nodiscard]] Result<std::optional<EntityId>> pick(const Vec2d& uv) const;
    [[nodiscard]] geometry::Bounds bounds(EntityId,const SceneSnapshot&) const;
    [[nodiscard]] bool camera_current(const Camera& camera) const { return published_camera_==camera.revision(); }
    void retry() { attempted_.reset(); asset_session_.reset(); }
    void release_texture();
    [[nodiscard]] ImTextureID texture() const { return reinterpret_cast<ImTextureID>(descriptor_); }
    [[nodiscard]] const graphics::Image& image() const { return image_; }
    [[nodiscard]] const graphics::ImageView& image_view() const { return view_; }
    [[nodiscard]] std::uint64_t pixel_signature() const { return pixel_signature_; }
    [[nodiscard]] const render::FrameInfo& info() const { return info_; }
    [[nodiscard]] bool current(const DocumentState& s) const { return published_session_==s.document_id && info_.revision==s.revision; }
    [[nodiscard]] const std::string& error() const { return error_; }
private:
    struct Key {
        DocumentId session; std::uint64_t revision; std::uint32_t width,height; bool srgb;
        std::uint64_t camera,preview;
        bool operator==(const Key&) const = default;
    };
    memory::ResourceHandle heap_;
    graphics::SubmissionQueue& queue_;
    render::ScenePipeline pipeline_;
    std::optional<DocumentId> asset_session_,published_session_;
    std::optional<Key> attempted_;
    render::GpuAssets assets_;
    graphics::Image image_;
    graphics::ImageView view_;
    VkDescriptorSet descriptor_ = VK_NULL_HANDLE;
    render::FrameInfo info_{};
    std::string error_;
    std::uint64_t pixel_signature_ = 0;
    std::uint64_t published_camera_ = 0;
    Camera camera_;
    render::RenderScene scene_;
    struct PickMesh { AssetId id; geometry::MeshQuery query; };
    std::vector<PickMesh> meshes_;
};
}

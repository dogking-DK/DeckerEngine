#pragma once
#include <dk/scene/SceneSnapshot.hpp>
#include <dk/memory/Resource.hpp>

namespace dk::render {
namespace detail { struct SceneState; }
struct RenderEntity {
    EntityId id;
    Transformd world;
    std::size_t first_asset = 0, asset_count = 0;
};
struct LocalTransformOverride { EntityId entity; Trsd local; };
// Shared immutable extraction; all returned spans borrow this owner.
class RenderScene final {
public:
    RenderScene() = default;
    [[nodiscard]] static Result<RenderScene> extract(memory::ResourceHandle, const SceneSnapshot&,
        const std::optional<LocalTransformOverride>& preview = {});
    [[nodiscard]] explicit operator bool() const noexcept { return bool(state_); }
    [[nodiscard]] SceneId id() const noexcept;
    [[nodiscard]] std::uint64_t revision() const noexcept;
    [[nodiscard]] std::span<const RenderEntity> entities() const noexcept;
    [[nodiscard]] Result<std::span<const AssetReference>> assets(std::size_t entity_index) const;
private:
    explicit RenderScene(std::shared_ptr<const detail::SceneState> state) : state_(std::move(state)) {}
    std::shared_ptr<const detail::SceneState> state_;
};
struct ViewDescription {
    std::uint32_t width = 0, height = 0;
    std::uint64_t frame = 0;
    Transformd camera_world;
    Mat4d projection = Mat4d::Identity(); // Caller supplies Vulkan clip-space conventions.
};
class RenderView final {
public:
    [[nodiscard]] static Result<RenderView> create(RenderScene, const ViewDescription&);
    [[nodiscard]] const RenderScene& scene() const noexcept { return scene_; }
    [[nodiscard]] const ViewDescription& description() const noexcept { return description_; }
    [[nodiscard]] const Transformd& world_to_view() const noexcept { return world_to_view_; }
    [[nodiscard]] const Mat4d& world_to_clip() const noexcept { return world_to_clip_; }
private:
    RenderView(RenderScene scene, const ViewDescription& desc, Transformd view, Mat4d clip)
        : scene_(std::move(scene)), description_(desc), world_to_view_(std::move(view)), world_to_clip_(std::move(clip)) {}
    RenderScene scene_;
    ViewDescription description_;
    Transformd world_to_view_;
    Mat4d world_to_clip_;
};
} // namespace dk::render

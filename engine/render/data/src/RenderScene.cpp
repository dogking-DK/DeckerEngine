#include <dk/render/RenderScene.hpp>
#include <dk/memory/Containers.hpp>
#include <dk/memory/SmartPtr.hpp>
#include <Eigen/LU>
#include <algorithm>
#include <new>

namespace dk::render {
namespace detail {
struct SceneState {
    explicit SceneState(memory::ResourceHandle heap)
        : entities(0, memory::Allocator<RenderEntity>{heap}), references(0, memory::Allocator<AssetReference>{heap}) {}
    SceneId id;
    std::uint64_t revision = 0;
    Vector<RenderEntity> entities;
    Vector<AssetReference> references;
};
}
Result<RenderScene> RenderScene::extract(memory::ResourceHandle heap, const SceneSnapshot& source,
    const std::optional<LocalTransformOverride>& preview) {
    if (!heap || heap.state() != memory::ResourceState::open)
        return std::unexpected(Error{ErrorCode::invalid_state, "render extraction requires open memory"});
    try {
        if (preview && std::ranges::none_of(source.entities(),[&](const auto& e) { return e.id==preview->entity; }))
            return std::unexpected(Error{ErrorCode::not_found,"Preview entity missing"});
        auto output = memory::make_shared_in<detail::SceneState>(heap, heap);
        output->id = source.id(); output->revision = source.revision();
        struct Node { const EntityData* source; Transformd world; unsigned state = 0; };
        Vector<Node> nodes(0, memory::Allocator<Node>{heap});
        nodes.reserve(source.entities().size());
        for (const auto& entity : source.entities()) nodes.push_back({&entity, {}, 0});
        std::ranges::sort(nodes, {}, [](const Node& node) { return node.source->id; });
        Vector<std::size_t> chain(0, memory::Allocator<std::size_t>{heap});
        chain.reserve(nodes.size());
        auto parent_index = [&](EntityId id) -> Result<std::size_t> {
            const auto found = std::ranges::lower_bound(nodes, id, {}, [](const Node& n) { return n.source->id; });
            if (found == nodes.end() || found->source->id != id)
                return std::unexpected(Error{ErrorCode::invalid_argument, "render snapshot has missing parent"});
            return static_cast<std::size_t>(found - nodes.begin());
        };
        for (std::size_t i = 0; i < nodes.size(); ++i) {
            auto current = i;
            while (nodes[current].state != 2) {
                auto& node = nodes[current];
                if (node.state == 1) return std::unexpected(Error{ErrorCode::invalid_argument, "render snapshot contains a cycle"});
                node.state = 1; chain.push_back(current);
                if (!node.source->parent) break;
                auto parent = parent_index(*node.source->parent);
                if (!parent) return std::unexpected(parent.error());
                current = *parent;
            }
            while (!chain.empty()) {
                auto& node = nodes[chain.back()]; chain.pop_back();
                auto world = Transformd::from_trs(preview && node.source->id==preview->entity ? preview->local : node.source->local);
                if (!world) return std::unexpected(world.error());
                if (node.source->parent) {
                    auto parent = parent_index(*node.source->parent);
                    if (!parent) return std::unexpected(parent.error());
                    world = nodes[*parent].world.compose(*world);
                    if (!world) return std::unexpected(world.error());
                }
                node.world = *world; node.state = 2;
            }
        }
        output->entities.reserve(nodes.size());
        for (const auto& node : nodes) {
            output->entities.push_back({node.source->id, node.world, output->references.size(), node.source->assets.size()});
            output->references.insert(output->references.end(), node.source->assets.begin(), node.source->assets.end());
        }
        return RenderScene{std::move(output)};
    } catch (const std::bad_alloc&) {
        return std::unexpected(Error{ErrorCode::internal_error, "render extraction allocation failed"});
    }
}
SceneId RenderScene::id() const noexcept { return state_ ? state_->id : SceneId{}; }
std::uint64_t RenderScene::revision() const noexcept { return state_ ? state_->revision : 0; }
std::span<const RenderEntity> RenderScene::entities() const noexcept { return state_ ? std::span<const RenderEntity>{state_->entities} : std::span<const RenderEntity>{}; }
Result<std::span<const AssetReference>> RenderScene::assets(std::size_t index) const {
    if (!state_ || index >= state_->entities.size())
        return std::unexpected(Error{ErrorCode::invalid_argument, "render entity index out of range"});
    const auto& entity = state_->entities[index];
    return std::span<const AssetReference>{state_->references}.subspan(entity.first_asset, entity.asset_count);
}
Result<RenderView> RenderView::create(RenderScene scene, const ViewDescription& desc) {
    if (!scene || !desc.width || !desc.height || !desc.projection.allFinite() || !desc.projection.fullPivLu().isInvertible())
        return std::unexpected(Error{ErrorCode::invalid_argument, "render view requires scene, extent and finite invertible projection"});
    auto view = desc.camera_world.inverse();
    if (!view) return std::unexpected(view.error());
    Mat4d clip = desc.projection * view->matrix();
    if (!clip.allFinite()) return std::unexpected(Error{ErrorCode::invalid_argument, "render view projection overflow"});
    return RenderView{std::move(scene), desc, *view, std::move(clip)};
}
} // namespace dk::render

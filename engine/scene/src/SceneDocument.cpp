#include <dk/scene/SceneDocument.hpp>

#include <flecs.h>

#include <algorithm>
#include <limits>
#include <unordered_map>

namespace dk {
namespace detail {
struct SceneIdentity { EntityId id; };
} // namespace detail
namespace {

std::unexpected<Error> scene_error(ErrorCode code, std::string message, std::string operation)
{
    return std::unexpected(Error{code, std::move(message), {std::move(operation)}});
}

} // namespace

struct SceneDocument::Impl {
    explicit Impl(SceneId id_value) : id{id_value} { world.component<detail::SceneIdentity>(); }
    flecs::world world;
    std::unordered_map<EntityId, flecs::entity_t> entities;
    SceneId id;
    std::uint64_t revision = 0;
    bool dirty = true;

    Result<void> can_edit() const
    {
        if (revision == std::numeric_limits<std::uint64_t>::max()) {
            return scene_error(ErrorCode::invalid_state, "Scene revision exhausted", "SceneDocument.edit");
        }
        return {};
    }
    void changed() noexcept { ++revision; dirty = true; }
};

SceneDocument::SceneDocument(std::unique_ptr<Impl> impl) : impl_{std::move(impl)} {}
SceneDocument::~SceneDocument() = default;

Result<std::unique_ptr<SceneDocument>> SceneDocument::create()
{
    const auto id = SceneId::generate();
    if (!id) { return std::unexpected(id.error().with_context("SceneDocument.create")); }
    return create(*id);
}

Result<std::unique_ptr<SceneDocument>> SceneDocument::create(SceneId id)
{
    if (id.is_nil()) { return scene_error(ErrorCode::invalid_argument, "Scene ID cannot be nil", "SceneDocument.create"); }
    return std::unique_ptr<SceneDocument>{new SceneDocument{std::make_unique<Impl>(id)}};
}

SceneId SceneDocument::id() const noexcept { return impl_->id; }
std::uint64_t SceneDocument::revision() const noexcept { return impl_->revision; }
bool SceneDocument::dirty() const noexcept { return impl_->dirty; }
std::size_t SceneDocument::entity_count() const noexcept { return impl_->entities.size(); }
bool SceneDocument::contains(EntityId id) const noexcept { return impl_->entities.contains(id); }

std::vector<EntityId> SceneDocument::entity_ids() const
{
    std::vector<EntityId> ids;
    ids.reserve(impl_->entities.size());
    for (const auto& [id, entity] : impl_->entities) { (void)entity; ids.push_back(id); }
    std::sort(ids.begin(), ids.end());
    return ids;
}

Result<EntityId> SceneDocument::create_entity()
{
    const auto id = EntityId::generate();
    if (!id) { return std::unexpected(id.error().with_context("SceneDocument.create_entity")); }
    const auto result = create_entity(*id);
    if (!result) { return std::unexpected(result.error()); }
    return *id;
}

Result<void> SceneDocument::create_entity(EntityId id)
{
    if (id.is_nil() || contains(id)) {
        return scene_error(ErrorCode::invalid_argument, "Entity ID is nil or already exists: " + id.to_string(),
            "SceneDocument.create_entity");
    }
    const auto editable = impl_->can_edit();
    if (!editable) { return editable; }
    const auto [slot, inserted] = impl_->entities.emplace(id, 0);
    (void)inserted; // Prevalidated above; the document has one serialized owner.
    try {
        auto entity = impl_->world.entity();
        if (entity.id() == 0) {
            auto error = scene_error(ErrorCode::internal_error, "ECS entity creation failed", "SceneDocument.create_entity");
            impl_->entities.erase(slot);
            return error;
        }
        try {
            entity.set<detail::SceneIdentity>({id});
        } catch (...) {
            entity.destruct();
            throw;
        }
        slot->second = entity.id();
    } catch (...) {
        impl_->entities.erase(slot);
        throw;
    }
    impl_->changed();
    return {};
}

Result<void> SceneDocument::destroy_entity(EntityId id)
{
    const auto found = impl_->entities.find(id);
    if (found == impl_->entities.end()) {
        return scene_error(ErrorCode::not_found, "Entity not found: " + id.to_string(), "SceneDocument.destroy_entity");
    }
    const auto editable = impl_->can_edit();
    if (!editable) { return editable; }
    impl_->world.entity(found->second).destruct();
    impl_->entities.erase(found);
    impl_->changed();
    return {};
}

Result<void> SceneDocument::validate() const
{
    if (static_cast<std::size_t>(impl_->world.count<detail::SceneIdentity>()) != entity_count()) {
        return scene_error(ErrorCode::internal_error, "Entity index and ECS count disagree", "SceneDocument.validate");
    }
    for (const auto& [id, handle] : impl_->entities) {
        const auto entity = impl_->world.entity(handle);
        if (handle == 0 || !entity.is_alive()) {
            return scene_error(ErrorCode::internal_error, "Entity index contains a dead handle", "SceneDocument.validate");
        }
        const auto* identity = entity.try_get<detail::SceneIdentity>();
        if (!identity || identity->id != id || id.is_nil()) {
            return scene_error(ErrorCode::internal_error, "Entity identity disagrees with index", "SceneDocument.validate");
        }
    }
    return {};
}

} // namespace dk

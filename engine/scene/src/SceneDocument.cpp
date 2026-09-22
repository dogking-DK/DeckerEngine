#include <dk/scene/SceneDocument.hpp>
#include <dk/math/Math.hpp>
#include "ScenePersistence.hpp"

#include <flecs.h>

#include <algorithm>
#include <limits>
#include <unordered_map>

namespace dk {
namespace detail {
struct SceneIdentity { EntityId id; };
struct SceneValue { EntityData data; Transformd world; };
struct SceneComponents { std::shared_ptr<const SceneValue> value; };
} // namespace detail
namespace {

std::unexpected<Error> scene_error(ErrorCode code, std::string message, std::string operation)
{
    return std::unexpected(Error{code, std::move(message), {std::move(operation)}});
}

bool valid_name(std::string_view text)
{
    if (text.size() > 1024) { return false; }
    for (std::size_t i = 0; i < text.size();) {
        auto ch = static_cast<unsigned char>(text[i++]);
        if (ch == 0) { return false; }
        if (ch < 0x80) { continue; }
        unsigned remaining = 0;
        std::uint32_t value = 0;
        std::uint32_t minimum = 0;
        if (ch >= 0xc2 && ch <= 0xdf) { remaining = 1; value = ch & 0x1fU; minimum = 0x80; }
        else if (ch >= 0xe0 && ch <= 0xef) { remaining = 2; value = ch & 0xfU; minimum = 0x800; }
        else if (ch >= 0xf0 && ch <= 0xf4) { remaining = 3; value = ch & 7U; minimum = 0x10000; }
        else { return false; }
        if (text.size() - i < remaining) { return false; }
        while (remaining-- != 0) {
            ch = static_cast<unsigned char>(text[i++]);
            if ((ch & 0xc0U) != 0x80U) { return false; }
            value = (value << 6U) | (ch & 0x3fU);
        }
        if (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) { return false; }
    }
    return true;
}

bool same_trs(const Trsd& a, const Trsd& b)
{
    return a.translation == b.translation && a.scale == b.scale && a.rotation.coeffs() == b.rotation.coeffs();
}

} // namespace

struct SceneDocument::Impl {
    explicit Impl(SceneId id_value) : id{id_value}
    {
        world.component<detail::SceneIdentity>();
        world.component<detail::SceneComponents>();
    }
    flecs::world world;
    std::unordered_map<EntityId, flecs::entity_t> entities;
    SceneId id;
    std::uint64_t revision = 0;
    bool dirty = true;
    std::shared_ptr<const int> origin = std::make_shared<const int>(0);

    Result<void> can_edit() const
    {
        if (revision == std::numeric_limits<std::uint64_t>::max()) {
            return scene_error(ErrorCode::invalid_state, "Scene revision exhausted", "SceneDocument.edit");
        }
        return {};
    }
    void changed() noexcept { ++revision; dirty = true; }

    const detail::SceneValue& value(EntityId entity_id) const
    {
        return *world.entity(entities.at(entity_id)).get<detail::SceneComponents>().value;
    }
    using Values = std::unordered_map<EntityId, std::shared_ptr<const detail::SceneValue>>;
    Result<Values> prepare(EntityId changed_id, const EntityData* replacement) const
    {
        std::unordered_map<EntityId, std::vector<EntityId>> children;
        std::vector<EntityId> ready;
        const auto data_for = [&](EntityId key) -> const EntityData& {
            return replacement && key == changed_id ? *replacement : value(key).data;
        };
        for (const auto& [key, handle] : entities) {
            (void)handle;
            const auto& data = data_for(key);
            if (data.parent) {
                if (!entities.contains(*data.parent)) {
                    return scene_error(ErrorCode::not_found, "Parent entity not found", "SceneDocument.hierarchy");
                }
                children[*data.parent].push_back(key);
            } else { ready.push_back(key); }
        }
        Values prepared;
        prepared.reserve(entities.size());
        for (std::size_t index = 0; index < ready.size(); ++index) {
            const auto key = ready[index];
            const auto& data = data_for(key);
            auto transform = Transformd::from_trs(data.local);
            if (!transform) { return std::unexpected(transform.error().with_context("SceneDocument.local")); }
            if (data.parent) { transform = prepared.at(*data.parent)->world.compose(*transform); }
            if (!transform) { return std::unexpected(transform.error().with_context("SceneDocument.world")); }
            prepared.emplace(key, std::make_shared<const detail::SceneValue>(detail::SceneValue{data, *transform}));
            if (const auto found = children.find(key); found != children.end()) {
                ready.insert(ready.end(), found->second.begin(), found->second.end());
            }
        }
        if (prepared.size() != entities.size()) {
            return scene_error(ErrorCode::invalid_argument, "Hierarchy contains a cycle", "SceneDocument.hierarchy");
        }
        return prepared;
    }
    Result<void> replace_hierarchy(const EntityData& data)
    {
        const auto editable = can_edit();
        if (!editable) { return editable; }
        auto prepared = prepare(data.id, &data);
        if (!prepared) { return std::unexpected(prepared.error()); }
        for (auto& [key, value_ptr] : *prepared) {
            world.entity(entities.at(key)).get_mut<detail::SceneComponents>().value.swap(value_ptr);
        }
        changed();
        return {};
    }
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
    if (entity_count() >= 10000) {
        return scene_error(ErrorCode::invalid_state, "Scene entity limit is 10000", "SceneDocument.create_entity");
    }
    if (id.is_nil() || contains(id)) {
        return scene_error(ErrorCode::invalid_argument, "Entity ID is nil or already exists: " + id.to_string(),
            "SceneDocument.create_entity");
    }
    const auto editable = impl_->can_edit();
    if (!editable) { return editable; }
    EntityData data;
    data.id = id;
    auto components = std::make_shared<const detail::SceneValue>(detail::SceneValue{std::move(data), {}});
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
            entity.set<detail::SceneComponents>({std::move(components)});
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
    for (const auto& [key, handle] : impl_->entities) {
        (void)handle;
        if (impl_->value(key).data.parent == id) {
            return scene_error(ErrorCode::invalid_state, "Cannot delete an entity with children", "SceneDocument.destroy_entity");
        }
    }
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
        const auto* components = entity.try_get<detail::SceneComponents>();
        if (!components || !components->value || components->value->data.id != id || !valid_name(components->value->data.name)) {
            return scene_error(ErrorCode::internal_error, "Entity components are inconsistent", "SceneDocument.validate");
        }
        const auto references = validate_asset_references(components->value->data.assets);
        if (!references) { return references; }
    }
    const auto prepared = impl_->prepare({}, nullptr);
    if (!prepared) { return std::unexpected(prepared.error()); }
    for (const auto& [key, candidate] : *prepared) {
        if (candidate->world.matrix() != impl_->value(key).world.matrix()) {
            return scene_error(ErrorCode::internal_error, "World transform cache is inconsistent", "SceneDocument.validate");
        }
    }
    return {};
}

Result<EntityData> SceneDocument::entity(EntityId id) const
{
    if (!contains(id)) { return scene_error(ErrorCode::not_found, "Entity not found", "SceneDocument.entity"); }
    return impl_->value(id).data;
}

Result<Transformd> SceneDocument::world_transform(EntityId id) const
{
    if (!contains(id)) { return scene_error(ErrorCode::not_found, "Entity not found", "SceneDocument.world_transform"); }
    return impl_->value(id).world;
}

Result<void> SceneDocument::set_name(EntityId id, std::string name)
{
    auto data = entity(id);
    if (!data) { return std::unexpected(data.error()); }
    if (!valid_name(name)) { return scene_error(ErrorCode::invalid_argument, "Name must be UTF-8 without NUL and at most 1024 bytes", "SceneDocument.set_name"); }
    if (data->name == name) { return {}; }
    const auto editable = impl_->can_edit();
    if (!editable) { return editable; }
    data->name = std::move(name);
    auto prepared = std::make_shared<const detail::SceneValue>(detail::SceneValue{std::move(*data), impl_->value(id).world});
    impl_->world.entity(impl_->entities.at(id)).get_mut<detail::SceneComponents>().value.swap(prepared);
    impl_->changed();
    return {};
}

Result<void> SceneDocument::set_local_transform(EntityId id, const Trsd& local)
{
    auto data = entity(id);
    if (!data) { return std::unexpected(data.error()); }
    if (same_trs(data->local, local)) { return {}; }
    const auto checked = Transformd::from_trs(local);
    if (!checked) { return std::unexpected(checked.error()); }
    auto normalized = local;
    normalized.rotation = *normalize_quaternion(local.rotation);
    if (same_trs(data->local, normalized)) { return {}; }
    data->local = normalized;
    return impl_->replace_hierarchy(*data);
}

Result<void> SceneDocument::set_parent(EntityId id, std::optional<EntityId> parent)
{
    auto data = entity(id);
    if (!data) { return std::unexpected(data.error()); }
    if (parent && parent->is_nil()) { return scene_error(ErrorCode::invalid_argument, "Parent ID cannot be nil", "SceneDocument.set_parent"); }
    if (data->parent == parent) { return {}; }
    data->parent = parent;
    return impl_->replace_hierarchy(*data);
}

Result<void> SceneDocument::set_asset_references(EntityId id, std::vector<AssetReference> references)
{
    auto data = entity(id);
    if (!data) { return std::unexpected(data.error()); }
    const auto valid = validate_asset_references(references);
    if (!valid) { return valid; }
    if (data->assets == references) { return {}; }
    const auto editable = impl_->can_edit();
    if (!editable) { return editable; }
    data->assets = std::move(references);
    auto prepared = std::make_shared<const detail::SceneValue>(detail::SceneValue{std::move(*data), impl_->value(id).world});
    impl_->world.entity(impl_->entities.at(id)).get_mut<detail::SceneComponents>().value.swap(prepared);
    impl_->changed();
    return {};
}

Result<SceneSnapshot> SceneDocument::snapshot() const
{
    const auto valid = validate();
    if (!valid) { return std::unexpected(valid.error()); }
    std::vector<EntityData> data;
    data.reserve(entity_count());
    for (const auto key : entity_ids()) { data.push_back(impl_->value(key).data); }
    return SceneSnapshot{id(), revision(), std::move(data), impl_->origin};
}

bool SceneSnapshot::same_content(const SceneSnapshot& other) const noexcept
{
    if (id_ != other.id_ || entities_.size() != other.entities_.size()) return false;
    for (std::size_t i = 0; i < entities_.size(); ++i) {
        const auto& a = entities_[i]; const auto& b = other.entities_[i];
        if (a.id != b.id || a.name != b.name || !same_trs(a.local, b.local) || a.parent != b.parent || a.assets != b.assets) return false;
    }
    return true;
}
std::size_t SceneSnapshot::logical_bytes() const noexcept
{
    auto size = sizeof(SceneSnapshot) + entities_.size() * sizeof(EntityData);
    for (const auto& entity : entities_) size += entity.name.size() + entity.assets.size() * sizeof(AssetReference);
    return size;
}
Result<std::unique_ptr<SceneDocument>> SceneDocument::stage(const SceneSnapshot& snapshot)
{
    return detail::ScenePersistence::build(snapshot.id(), 0, {snapshot.entities().begin(), snapshot.entities().end()});
}
Result<bool> SceneDocument::apply_snapshot(const SceneSnapshot& desired)
{
    if (desired.id() != id()) return scene_error(ErrorCode::invalid_argument, "Snapshot belongs to another scene", "SceneDocument.apply_snapshot");
    auto current = snapshot(); if (!current) return std::unexpected(current.error());
    if (current->same_content(desired)) return false;
    auto editable = impl_->can_edit(); if (!editable) return std::unexpected(editable.error());
    auto candidate = detail::ScenePersistence::build(id(), revision() + 1, {desired.entities().begin(), desired.entities().end()});
    if (!candidate) return std::unexpected(candidate.error());
    (*candidate)->impl_->origin = impl_->origin;
    impl_.swap((*candidate)->impl_);
    return true;
}

Result<std::unique_ptr<SceneDocument>> detail::ScenePersistence::build(
    SceneId id, std::uint64_t revision, std::vector<EntityData> entities)
{
    if (entities.size() > 10000) {
        return scene_error(ErrorCode::invalid_argument, "Scene entity limit is 10000", "SceneDocument.import");
    }
    auto result = SceneDocument::create(id);
    if (!result) { return result; }
    auto& doc = **result;
    // Pass 1: register all persistent IDs and local data without resolving parents.
    for (auto& data : entities) {
        if (!valid_name(data.name)) {
            return scene_error(ErrorCode::invalid_argument, "Invalid entity name", "SceneDocument.import");
        }
        const auto references = validate_asset_references(data.assets);
        if (!references) { return std::unexpected(references.error()); }
        const auto transform = Transformd::from_trs(data.local);
        if (!transform) { return std::unexpected(transform.error()); }
        const auto normalized = *normalize_quaternion(data.local.rotation);
        // Preserve already-normalized serialized coefficients at their original precision.
        if ((data.local.rotation.coeffs() - normalized.coeffs()).cwiseAbs().maxCoeff()
            > 8.0 * std::numeric_limits<double>::epsilon()) {
            data.local.rotation = normalized;
        }
        const auto added = doc.create_entity(data.id);
        if (!added) { return std::unexpected(added.error()); }
        auto prepared = std::make_shared<const SceneValue>(SceneValue{data, {}});
        doc.impl_->world.entity(doc.impl_->entities.at(data.id)).get_mut<SceneComponents>().value.swap(prepared);
    }
    // Pass 2: resolve all parent IDs and build finite world transforms in topological order.
    auto prepared = doc.impl_->prepare({}, nullptr);
    if (!prepared) { return std::unexpected(prepared.error()); }
    for (auto& [key, value] : *prepared) {
        doc.impl_->world.entity(doc.impl_->entities.at(key)).get_mut<SceneComponents>().value.swap(value);
    }
    doc.impl_->revision = revision;
    return result;
}

bool detail::ScenePersistence::owns(const SceneDocument& document, const SceneSnapshot& snapshot) noexcept
{
    return document.impl_->origin == snapshot.origin_;
}
void detail::ScenePersistence::saved(SceneDocument& document, const SceneSnapshot& snapshot) noexcept
{
    document.impl_->dirty = document.revision() != snapshot.revision();
}
void detail::ScenePersistence::loaded(SceneDocument& document) noexcept { document.impl_->dirty = false; }

} // namespace dk

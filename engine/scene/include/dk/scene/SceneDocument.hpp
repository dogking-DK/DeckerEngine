#pragma once

#include <dk/core/StableId.hpp>
#include <dk/scene/Components.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace dk {

// Single-owner editing document. No ECS handles or mutable component references
// escape this boundary; all operations must be serialized by the caller.
class SceneDocument {
public:
    [[nodiscard]] static Result<std::unique_ptr<SceneDocument>> create();
    [[nodiscard]] static Result<std::unique_ptr<SceneDocument>> create(SceneId id);
    ~SceneDocument();
    SceneDocument(const SceneDocument&) = delete;
    SceneDocument& operator=(const SceneDocument&) = delete;
    SceneDocument(SceneDocument&&) = delete;
    SceneDocument& operator=(SceneDocument&&) = delete;

    [[nodiscard]] SceneId id() const noexcept;
    [[nodiscard]] std::uint64_t revision() const noexcept;
    [[nodiscard]] bool dirty() const noexcept;
    [[nodiscard]] std::size_t entity_count() const noexcept;
    [[nodiscard]] bool contains(EntityId id) const noexcept;
    [[nodiscard]] std::vector<EntityId> entity_ids() const;

    [[nodiscard]] Result<EntityId> create_entity();
    [[nodiscard]] Result<void> create_entity(EntityId id);
    [[nodiscard]] Result<void> destroy_entity(EntityId id);
    [[nodiscard]] Result<EntityData> entity(EntityId id) const;
    [[nodiscard]] Result<Transformd> world_transform(EntityId id) const;
    [[nodiscard]] Result<void> set_name(EntityId id, std::string name);
    [[nodiscard]] Result<void> set_local_transform(EntityId id, const Trsd& local);
    // Retains the local TRS. nullopt detaches; parents with children cannot be deleted.
    [[nodiscard]] Result<void> set_parent(EntityId id, std::optional<EntityId> parent);
    [[nodiscard]] Result<void> validate() const;

private:
    struct Impl;
    explicit SceneDocument(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace dk

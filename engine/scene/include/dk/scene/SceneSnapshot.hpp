#pragma once
#include <dk/scene/Components.hpp>
#include <memory>
#include <span>
#include <vector>

namespace dk {
namespace detail { struct ScenePersistence; }
class SceneDocument;
class SceneSnapshot {
public:
    [[nodiscard]] SceneId id() const noexcept { return id_; }
    [[nodiscard]] std::uint64_t revision() const noexcept { return revision_; }
    [[nodiscard]] std::span<const EntityData> entities() const noexcept { return entities_; }
private:
    friend class SceneDocument;
    friend struct detail::ScenePersistence;
    SceneSnapshot(SceneId id, std::uint64_t revision, std::vector<EntityData> entities,
        std::shared_ptr<const int> origin)
        : id_{id}, revision_{revision}, entities_{std::move(entities)}, origin_{std::move(origin)} {}
    SceneId id_;
    std::uint64_t revision_;
    std::vector<EntityData> entities_;
    std::shared_ptr<const int> origin_;
};
} // namespace dk

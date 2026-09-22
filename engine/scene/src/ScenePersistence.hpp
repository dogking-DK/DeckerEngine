#pragma once
#include <dk/scene/SceneDocument.hpp>
namespace dk::detail {
struct ScenePersistence {
    static Result<std::unique_ptr<SceneDocument>> build(SceneId id, std::uint64_t revision, std::vector<EntityData> entities);
    static bool owns(const SceneDocument& document, const SceneSnapshot& snapshot) noexcept;
    static void saved(SceneDocument& document, const SceneSnapshot& snapshot) noexcept;
    static void loaded(SceneDocument& document) noexcept;
};
} // namespace dk::detail

#pragma once
#include <dk/services/SceneService.hpp>

namespace dk::editor {
// A detached preview. Only Workspace::commit_transform publishes it through a guarded command.
struct TransformEdit {
    EditGuard guard;
    EntityId entity;
    Trsd original,value;
    Transformd parent;
    [[nodiscard]] Result<void> translate(const Vec3d& world_delta);
    [[nodiscard]] Result<void> rotate(unsigned axis,double radians);
    [[nodiscard]] Result<void> scale(unsigned axis,double factor);
    [[nodiscard]] bool changed() const;
    [[nodiscard]] Result<Transformd> world() const;
};
[[nodiscard]] Result<Transformd> entity_world(const SceneSnapshot&,EntityId);
}

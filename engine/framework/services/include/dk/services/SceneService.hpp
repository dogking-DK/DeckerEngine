#pragma once
#include <dk/scene/SceneIO.hpp>
#include <variant>

namespace dk
{
struct DocumentIdTag;
using DocumentId = StableId<DocumentIdTag>;
struct EditGuard
{
    DocumentId document_id;
    std::uint64_t revision;
};
struct DocumentState
{
    DocumentId document_id;
    SceneId scene_id;
    std::uint64_t revision;
    bool dirty;
    std::size_t entity_count;
};
struct CreateEntity
{
    std::optional<EntityId> id;
};
struct DeleteEntity
{
    EntityId id;
};
struct SetName
{
    EntityId id;
    std::string name;
};
struct SetTransform
{
    EntityId id;
    Trsd transform;
};
struct SetParent
{
    EntityId id;
    std::optional<EntityId> parent;
};
struct SetAssets
{
    EntityId id;
    std::vector<AssetReference> assets;
};
using SceneEdit = std::variant<CreateEntity, DeleteEntity, SetName, SetTransform, SetParent, SetAssets>;
class SceneService final
{
  public:
    [[nodiscard]] static Result<std::unique_ptr<SceneService>> create(const std::filesystem::path &root);
    [[nodiscard]] Result<DocumentState> state() const;
    [[nodiscard]] Result<const SceneDocument *> document() const;
    [[nodiscard]] Result<void> check_guard(EditGuard guard) const;
    [[nodiscard]] Result<void> new_scene(ProjectDescription description, std::optional<EditGuard> guard = {});
    [[nodiscard]] Result<void> load(const std::filesystem::path &manifest,
                                    std::optional<EditGuard> guard = {});
    [[nodiscard]] Result<void> save(EditGuard guard);
    [[nodiscard]] Result<void> save_manifest(EditGuard guard, const std::filesystem::path &manifest);
    [[nodiscard]] Result<std::optional<EntityId>> edit(EditGuard guard, const SceneEdit &edit);

  private:
    explicit SceneService(ProjectPaths paths) : paths_{std::move(paths)} {}
    [[nodiscard]] Result<void> check_replacement(std::optional<EditGuard> guard) const;
    [[nodiscard]] Result<std::optional<EntityId>> apply_edit(SceneDocument &document,
                                                             const SceneEdit &edit) const;
    ProjectPaths paths_;
    std::unique_ptr<Project> project_;
    std::unique_ptr<SceneDocument> document_;
    DocumentId document_id_;
};
} // namespace dk

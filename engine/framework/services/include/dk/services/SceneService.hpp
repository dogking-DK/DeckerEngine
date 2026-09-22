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
struct HistoryLimits
{
    std::size_t entries = 64;
    std::size_t logical_bytes = 32 * 1024 * 1024;
};
struct HistoryStatus
{
    std::size_t undo_count;
    std::size_t redo_count;
    std::size_t logical_bytes;
};
class SceneService final
{
  public:
    [[nodiscard]] static Result<std::unique_ptr<SceneService>> create(const std::filesystem::path &root,
                                                                      HistoryLimits limits = {});
    [[nodiscard]] Result<DocumentState> state() const;
    [[nodiscard]] Result<const SceneDocument *> document() const;
    [[nodiscard]] Result<void> check_guard(EditGuard guard) const;
    [[nodiscard]] Result<void> new_scene(ProjectDescription description, std::optional<EditGuard> guard = {});
    [[nodiscard]] Result<void> load(const std::filesystem::path &manifest,
                                    std::optional<EditGuard> guard = {});
    [[nodiscard]] Result<void> save(EditGuard guard);
    [[nodiscard]] Result<void> save_manifest(EditGuard guard, const std::filesystem::path &manifest);
    [[nodiscard]] Result<std::optional<EntityId>> edit(EditGuard guard, const SceneEdit &edit);
    [[nodiscard]] Result<std::vector<std::optional<EntityId>>> edit_batch(EditGuard guard,
                                                                          std::span<const SceneEdit> edits);
    [[nodiscard]] HistoryStatus history_status() const noexcept;
    [[nodiscard]] Result<void> undo(EditGuard guard);
    [[nodiscard]] Result<void> redo(EditGuard guard);

  private:
    explicit SceneService(ProjectPaths paths, HistoryLimits limits)
        : paths_{std::move(paths)}, limits_{limits}
    {
    }
    struct HistoryEntry
    {
        SceneSnapshot before;
        SceneSnapshot after;
        std::size_t bytes;
    };
    using History = std::vector<std::shared_ptr<const HistoryEntry>>;
    [[nodiscard]] Result<void> history_step(EditGuard guard, bool redo);
    [[nodiscard]] Result<void> check_replacement(std::optional<EditGuard> guard) const;
    [[nodiscard]] Result<std::optional<EntityId>> apply_edit(SceneDocument &document,
                                                             const SceneEdit &edit) const;
    ProjectPaths paths_;
    std::unique_ptr<Project> project_;
    std::unique_ptr<SceneDocument> document_;
    DocumentId document_id_;
    HistoryLimits limits_;
    History undo_;
    History redo_;
};
} // namespace dk

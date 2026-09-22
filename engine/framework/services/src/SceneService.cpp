#include <algorithm>
#include <cwctype>
#include <dk/services/SceneService.hpp>

namespace dk
{
Result<std::unique_ptr<SceneService>> SceneService::create(const std::filesystem::path &root,
                                                           HistoryLimits limits)
{
    if (limits.entries == 0 || limits.entries > 64 || limits.logical_bytes == 0 ||
        limits.logical_bytes > 32 * 1024 * 1024)
        return std::unexpected(Error{ErrorCode::invalid_argument, "History limits exceed supported range"});
    auto paths = ProjectPaths::create(root);
    if (!paths)
        return std::unexpected(paths.error());
    return std::unique_ptr<SceneService>(new SceneService(std::move(*paths), limits));
}
Result<const SceneDocument *> SceneService::document() const
{
    if (!document_)
        return std::unexpected(Error{ErrorCode::invalid_state, "No active scene"});
    return document_.get();
}
Result<DocumentState> SceneService::state() const
{
    auto doc = document();
    if (!doc)
        return std::unexpected(doc.error());
    return DocumentState{document_id_, document_->id(), document_->revision(), document_->dirty(),
                         document_->entity_count()};
}
Result<void> SceneService::check_guard(EditGuard guard) const
{
    auto current = state();
    if (!current)
        return std::unexpected(current.error());
    if (guard.document_id != current->document_id || guard.revision != current->revision)
        return std::unexpected(
            Error{ErrorCode::conflict,
                  "Stale document or revision",
                  {"expected=" + guard.document_id.to_string() + ":" + std::to_string(guard.revision),
                   "actual=" + current->document_id.to_string() + ":" + std::to_string(current->revision)}});
    return {};
}
Result<void> SceneService::check_replacement(std::optional<EditGuard> guard) const
{
    if (guard)
        return check_guard(*guard);
    if (document_)
        return std::unexpected(Error{ErrorCode::conflict, "Replacing the active scene requires a guard"});
    return {};
}
Result<void> SceneService::new_scene(ProjectDescription description, std::optional<EditGuard> guard)
{
    auto valid = check_replacement(guard);
    if (!valid)
        return valid;
    auto project = Project::create(paths_.root(), std::move(description));
    if (!project)
        return std::unexpected(project.error());
    auto document = SceneDocument::create();
    if (!document)
        return std::unexpected(document.error());
    auto id = DocumentId::generate();
    if (!id)
        return std::unexpected(id.error());
    auto owned_project = std::make_unique<Project>(std::move(*project));
    project_ = std::move(owned_project);
    document_ = std::move(*document);
    document_id_ = *id;
    undo_.clear();
    redo_.clear();
    return {};
}
Result<void> SceneService::load(const std::filesystem::path &manifest, std::optional<EditGuard> guard)
{
    auto valid = check_replacement(guard);
    if (!valid)
        return valid;
    auto project = Project::open(paths_.root(), manifest);
    if (!project)
        return std::unexpected(project.error());
    auto document = load_scene(*project);
    if (!document)
        return std::unexpected(document.error());
    auto id = DocumentId::generate();
    if (!id)
        return std::unexpected(id.error());
    auto owned_project = std::make_unique<Project>(std::move(*project));
    project_ = std::move(owned_project);
    document_ = std::move(*document);
    document_id_ = *id;
    undo_.clear();
    redo_.clear();
    return {};
}
Result<void> SceneService::save(EditGuard guard)
{
    auto valid = check_guard(guard);
    if (!valid)
        return valid;
    return save_scene(*document_, *project_);
}
Result<void> SceneService::save_manifest(EditGuard guard, const std::filesystem::path &manifest)
{
    auto valid = check_guard(guard);
    if (!valid)
        return valid;
    // A manifest must never overwrite its own scene file.
    auto target = paths_.resolve(manifest);
    if (!target)
        return std::unexpected(target.error());
    auto scene_path = project_->scene_path();
    if (!scene_path)
        return std::unexpected(scene_path.error());
    auto a = *target;
    auto b = *scene_path;
#ifdef _WIN32
    auto an = a.native();
    auto bn = b.native();
    std::transform(an.begin(), an.end(), an.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    std::transform(bn.begin(), bn.end(), bn.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    if (an == bn)
        return std::unexpected(Error{ErrorCode::invalid_argument, "Manifest and scene paths must differ"});
#else
    if (a == b)
        return std::unexpected(Error{ErrorCode::invalid_argument, "Manifest and scene paths must differ"});
#endif
    return save_project(*project_, manifest);
}
Result<std::optional<EntityId>> SceneService::apply_edit(SceneDocument &doc, const SceneEdit &edit) const
{
    return std::visit(
        [&](const auto &action) -> Result<std::optional<EntityId>>
        {
            using T = std::decay_t<decltype(action)>;
            Result<void> result;
            if constexpr (std::is_same_v<T, CreateEntity>)
            {
                auto id = action.id ? Result<EntityId>(*action.id) : EntityId::generate();
                if (!id)
                    return std::unexpected(id.error());
                result = doc.create_entity(*id);
                if (!result)
                    return std::unexpected(result.error());
                return std::optional<EntityId>(*id);
            }
            else
            {
                if constexpr (std::is_same_v<T, DeleteEntity>)
                    result = doc.destroy_entity(action.id);
                else if constexpr (std::is_same_v<T, SetName>)
                    result = doc.set_name(action.id, action.name);
                else if constexpr (std::is_same_v<T, SetTransform>)
                    result = doc.set_local_transform(action.id, action.transform);
                else if constexpr (std::is_same_v<T, SetParent>)
                    result = doc.set_parent(action.id, action.parent);
                else if constexpr (std::is_same_v<T, SetAssets>)
                {
                    for (const auto &asset : action.assets)
                    {
                        auto resolved = project_->resolve_asset(asset);
                        if (!resolved)
                            return std::unexpected(resolved.error());
                    }
                    result = doc.set_asset_references(action.id, action.assets);
                }
                if (!result)
                    return std::unexpected(result.error());
                return std::optional<EntityId>{};
            }
        },
        edit);
}
Result<std::optional<EntityId>> SceneService::edit(EditGuard guard, const SceneEdit &edit)
{
    auto result = edit_batch(guard, std::span{&edit, std::size_t{1}});
    if (!result)
        return std::unexpected(result.error());
    return result->front();
}
Result<std::vector<std::optional<EntityId>>> SceneService::edit_batch(EditGuard guard,
                                                                      std::span<const SceneEdit> edits)
{
    auto valid = check_guard(guard);
    if (!valid)
        return std::unexpected(valid.error());
    if (edits.empty() || edits.size() > 128)
        return std::unexpected(Error{ErrorCode::invalid_argument, "Transaction requires 1-128 edits"});
    auto before = document_->snapshot();
    if (!before)
        return std::unexpected(before.error());
    auto staged = SceneDocument::stage(*before);
    if (!staged)
        return std::unexpected(staged.error());
    std::vector<std::optional<EntityId>> results;
    results.reserve(edits.size());
    for (std::size_t i = 0; i < edits.size(); ++i)
    {
        auto result = apply_edit(**staged, edits[i]);
        if (!result)
            return std::unexpected(result.error().with_context("transaction index=" + std::to_string(i)));
        results.push_back(*result);
    }
    auto after = (*staged)->snapshot();
    if (!after)
        return std::unexpected(after.error());
    if (before->same_content(*after))
        return results;
    const auto bytes = before->logical_bytes() + after->logical_bytes();
    if (bytes > limits_.logical_bytes)
        return std::unexpected(Error{ErrorCode::invalid_state, "Transaction exceeds history byte budget"});
    auto entry =
        std::make_shared<const HistoryEntry>(HistoryEntry{std::move(*before), std::move(*after), bytes});
    auto prepared = undo_;
    prepared.push_back(entry);
    std::size_t total = 0;
    for (const auto &value : prepared)
        total += value->bytes;
    while (prepared.size() > limits_.entries || total > limits_.logical_bytes)
    {
        total -= prepared.front()->bytes;
        prepared.erase(prepared.begin());
    }
    auto applied = document_->apply_snapshot(entry->after);
    if (!applied)
        return std::unexpected(applied.error());
    if (*applied)
    {
        undo_.swap(prepared);
        redo_.clear();
    }
    return results;
}
HistoryStatus SceneService::history_status() const noexcept
{
    std::size_t bytes = 0;
    for (const auto &entry : undo_)
        bytes += entry->bytes;
    for (const auto &entry : redo_)
        bytes += entry->bytes;
    return {undo_.size(), redo_.size(), bytes};
}
Result<void> SceneService::history_step(EditGuard guard, bool redo)
{
    auto valid = check_guard(guard);
    if (!valid)
        return valid;
    const auto &source = redo ? redo_ : undo_;
    if (source.empty())
        return std::unexpected(Error{ErrorCode::invalid_state, redo ? "Nothing to redo" : "Nothing to undo"});
    auto prepared_undo = undo_;
    auto prepared_redo = redo_;
    auto entry = source.back();
    if (redo)
    {
        prepared_redo.pop_back();
        prepared_undo.push_back(entry);
    }
    else
    {
        prepared_undo.pop_back();
        prepared_redo.push_back(entry);
    }
    auto applied = document_->apply_snapshot(redo ? entry->after : entry->before);
    if (!applied)
        return std::unexpected(applied.error());
    undo_.swap(prepared_undo);
    redo_.swap(prepared_redo);
    return {};
}
Result<void> SceneService::undo(EditGuard guard)
{
    return history_step(guard, false);
}
Result<void> SceneService::redo(EditGuard guard)
{
    return history_step(guard, true);
}
} // namespace dk

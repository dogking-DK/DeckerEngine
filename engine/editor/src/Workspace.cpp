#include <dk/editor/Workspace.hpp>
#include <algorithm>

namespace dk::editor {
namespace {
Error pending_error() { return {ErrorCode::invalid_state,"Apply or Revert the Inspector draft first."}; }
Json guard_json(EditGuard guard) { return {{"document_id",guard.document_id.to_string()},{"revision",guard.revision}}; }
}
Result<std::unique_ptr<Workspace>> Workspace::create(const std::filesystem::path& root) {
    auto runtime=Runtime::create(root);
    if (!runtime) return std::unexpected(runtime.error());
    return std::unique_ptr<Workspace>(new Workspace(std::move(*runtime)));
}
Result<Json> Workspace::call(std::string_view method, Json p) {
    auto result=runtime_->dispatch(method,p);
    if (!result) return std::unexpected(result.error());
    return std::move(result->result);
}
Json Workspace::guard() const {
    return guard_json({snapshot_->state.document_id,snapshot_->state.revision});
}
Result<void> Workspace::open(const std::filesystem::path& manifest, bool discard) {
    if (dirty() && !discard) return std::unexpected(pending_error());
    auto path=path_to_utf8(manifest);
    if (!path) return std::unexpected(path.error());
    Json params={{"manifest",*path}};
    if (snapshot_) params["guard"]=guard();
    auto result=call("scene.load",std::move(params));
    if (!result) return std::unexpected(result.error());
    manifest_=manifest;
    selection_.reset(); draft_.reset();
    return refresh();
}
Result<void> Workspace::refresh() {
    auto query=call("scene.query",{{"limit",1}});
    if (!query) return std::unexpected(query.error());
    const auto& state=query->at("state");
    auto id=DocumentId::parse(state.at("document_id").get<std::string>());
    if (!id) return std::unexpected(id.error());
    auto candidate=runtime_->read_scene({*id,state.at("revision").get<std::uint64_t>()});
    if (!candidate) return std::unexpected(candidate.error());
    auto history=call("history.status");
    if (!history) return std::unexpected(history.error());
    if (!snapshot_ || snapshot_->state.document_id!=candidate->state.document_id) {
        selection_.reset(); draft_.reset();
    }
    snapshot_=std::move(*candidate);
    history_={history->at("undo_count").get<std::size_t>(),history->at("redo_count").get<std::size_t>(),
        history->at("logical_bytes").get<std::size_t>()};
    if (selection_) {
        const auto entities=snapshot_->scene.entities();
        if (std::ranges::none_of(entities,[&](const auto& e) { return e.id==selection_; })) {
            selection_.reset(); draft_.reset();
        } else if (!pending()) return revert();
    }
    return {};
}
Result<void> Workspace::select(std::optional<EntityId> id) {
    if (selection_==id) return {};
    if (pending()) return std::unexpected(pending_error());
    if (id && (!snapshot_ || std::ranges::none_of(snapshot_->scene.entities(),[&](const auto& e) { return e.id==id; })))
        return std::unexpected(Error{ErrorCode::not_found,"Selected entity is not in this scene."});
    selection_=id;
    return revert();
}
Result<void> Workspace::revert() {
    draft_.reset();
    if (!selection_ || !snapshot_) return {};
    for (const auto& entity : snapshot_->scene.entities()) if (entity.id==selection_) {
        draft_=InspectorDraft{{snapshot_->state.document_id,snapshot_->state.revision},entity,false};
        return {};
    }
    selection_.reset();
    return {};
}
Result<void> Workspace::apply() {
    if (!pending()) return {};
    const auto& e=draft_->entity; const auto& t=e.local;
    Json transform={{"translation",{t.translation.x(),t.translation.y(),t.translation.z()}},
        {"rotation",{t.rotation.x(),t.rotation.y(),t.rotation.z(),t.rotation.w()}},
        {"scale",{t.scale.x(),t.scale.y(),t.scale.z()}}};
    auto result=call("scene.transaction",{{"guard",guard_json(draft_->guard)},{"commands",Json::array({
        {{"method","entity.set_name"},{"params",{{"id",e.id.to_string()},{"name",e.name}}}},
        {{"method","entity.set_transform"},{"params",{{"id",e.id.to_string()},{"transform",transform}}}}
    })}});
    if (!result) return std::unexpected(result.error());
    draft_->modified=false;
    return refresh();
}
Result<void> Workspace::save() {
    if (!snapshot_) return std::unexpected(Error{ErrorCode::invalid_state,"Open a project before saving."});
    if (auto applied=apply(); !applied) return applied;
    auto result=call("scene.save",{{"guard",guard()}});
    if (!result) return std::unexpected(result.error());
    return refresh();
}
Result<void> Workspace::history_step(std::string_view method) {
    if (pending()) return std::unexpected(pending_error());
    if (!snapshot_) return std::unexpected(Error{ErrorCode::invalid_state,"No scene is open."});
    auto result=call(method,{{"guard",guard()}});
    if (!result) return std::unexpected(result.error());
    return refresh();
}
Result<void> Workspace::undo() { return history_step("history.undo"); }
Result<void> Workspace::redo() { return history_step("history.redo"); }
Result<TransformEdit> Workspace::begin_transform() const {
    if (pending()) return std::unexpected(pending_error());
    if (!draft_ || !snapshot_) return std::unexpected(Error{ErrorCode::invalid_state,"Select an entity before using Gizmo"});
    Transformd parent;
    if (draft_->entity.parent) {
        auto world=entity_world(snapshot_->scene,*draft_->entity.parent);
        if (!world) return std::unexpected(world.error());
        parent=*world;
    }
    if (auto inverse=parent.inverse(); !inverse) return std::unexpected(inverse.error());
    return TransformEdit{{snapshot_->state.document_id,snapshot_->state.revision},draft_->entity.id,draft_->entity.local,draft_->entity.local,parent};
}
Result<void> Workspace::commit_transform(const TransformEdit& edit) {
    if (pending()) return std::unexpected(pending_error());
    if (!snapshot_ || edit.guard.document_id!=snapshot_->state.document_id || edit.guard.revision!=snapshot_->state.revision)
        return std::unexpected(Error{ErrorCode::conflict,"Transform preview is stale"});
    if (selection_!=edit.entity) return std::unexpected(Error{ErrorCode::conflict,"Transform selection changed"});
    auto valid=Transformd::from_trs(edit.value); if (!valid) return std::unexpected(valid.error());
    if (!edit.changed()) return {};
    const auto& t=edit.value;
    auto result=call("entity.set_transform",{{"guard",guard_json(edit.guard)},{"id",edit.entity.to_string()},
        {"transform",{{"translation",{t.translation.x(),t.translation.y(),t.translation.z()}},
            {"rotation",{t.rotation.x(),t.rotation.y(),t.rotation.z(),t.rotation.w()}},{"scale",{t.scale.x(),t.scale.y(),t.scale.z()}}}}});
    if (!result) return std::unexpected(result.error());
    return refresh();
}
} // namespace dk::editor

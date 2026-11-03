#include <dk/editor/Workspace.hpp>
#include "ConsoleDispatch.hpp"
#include <algorithm>
#include <stdexcept>

namespace dk::editor {
Workspace::~Workspace() {
#ifdef _WIN32
    close_ipc();
#endif
    detach_editor_viewport();
}
Result<void> Workspace::attach_editor_viewport(EditorViewportCapabilities capabilities) {
    if(editor_viewport_) return std::unexpected(Error{ErrorCode::invalid_state,"Editor viewport already attached"});
    auto service=EditorViewportService::create(std::move(capabilities),[this]()->Result<EditorViewportContext> {
        if(stopping()) return std::unexpected(Error{ErrorCode::invalid_state,"Editor is stopping"});
        auto state=runtime_->document_state(); if(!state) return std::unexpected(state.error());
        return EditorViewportContext{state->document_id,state->revision,pending() ? "Apply or Revert pending Inspector/Environment drafts first" :
            viewport_interacting_ ? "Finish the active Edit Gizmo/navigation gesture first" : ""};
    });
    if(!service) return std::unexpected(service.error());
    auto attached=runtime_->attach_editor_viewport(*service); if(!attached) return attached;
    editor_viewport_=std::move(*service); return {};
}
void Workspace::detach_editor_viewport() { runtime_->detach_editor_viewport(); editor_viewport_.reset(); viewport_interacting_=false; }
ConsoleReferenceVersion Workspace::console_reference_version() const {
    if (stopping()) return {};
    return {snapshot_ ? snapshot_->state.document_id : DocumentId{},snapshot_ ? snapshot_->state.revision : 0,runtime_->asset_catalog_version(),snapshot_publication_};
}
Result<ConsoleReferencePage> Workspace::read_console_references(std::string_view provider,
    const ConsoleReferenceVersion& version, std::size_t offset, std::size_t limit) const {
    if (version!=console_reference_version()) return std::unexpected(Error{ErrorCode::conflict,"Reference source changed"});
    if (!limit || limit>256) return std::unexpected(Error{ErrorCode::invalid_argument,"Reference page limit must be 1..256"});
    if (stopping()) return std::unexpected(Error{ErrorCode::invalid_state,"Workspace is stopping"});
    if(provider=="scene.entity") {
        if (!snapshot_) return std::unexpected(Error{ErrorCode::invalid_state,"No scene for entity candidates"});
        const auto entities=snapshot_->scene.entities();
        if(offset>entities.size()) return std::unexpected(Error{ErrorCode::invalid_argument,"Entity page offset outside range"});
        const auto end=std::min(entities.size(),offset+limit);
        ConsoleReferencePage page{version,{},offset,entities.size(),end<entities.size()};
        for(auto i=offset;i<end;++i) page.records.push_back({entities[i].id.to_string(),entities[i].name,"entity"});
        return page;
    }
    if(provider=="assets.reference") {
        if(!snapshot_) return std::unexpected(Error{ErrorCode::invalid_state,"No project for asset references"});
        const auto& assets=snapshot_->project.description().assets;
        if(offset>assets.size()) return std::unexpected(Error{ErrorCode::invalid_argument,"Project asset offset outside range"});
        const auto end=std::min(assets.size(),offset+limit);
        ConsoleReferencePage page{version,{},offset,assets.size(),end<assets.size()};
        for(auto i=offset;i<end;++i) page.records.push_back({assets[i].id.to_string(),assets[i].path,std::string(asset_kind_name(assets[i].kind))});
        return page;
    }
    if(provider!="assets.any") return std::unexpected(Error{ErrorCode::invalid_argument,"Unknown reference provider"});
    auto assets=runtime_->read_asset_catalog(version.assets,offset,limit); if(!assets) return std::unexpected(assets.error());
    ConsoleReferencePage page{version,{},offset,assets->total,assets->has_more};
    for(const auto& asset : assets->records) page.records.push_back({asset.id.to_string(),asset.path,std::string(asset_kind_name(asset.kind))});
    return page;
}
namespace {
Error pending_error() { return {ErrorCode::invalid_state,"Apply or Revert the pending draft first."}; }
Json guard_json(EditGuard guard) { return {{"document_id",guard.document_id.to_string()},{"revision",guard.revision}}; }
}
Result<std::unique_ptr<Workspace>> Workspace::create(const std::filesystem::path& root) {
    auto runtime=Runtime::create(root);
    if (!runtime) return std::unexpected(runtime.error());
    return std::unique_ptr<Workspace>(new Workspace(std::move(*runtime)));
}
void Workspace::pump() {
    runtime_->pump();
#ifdef _WIN32
    if (ipc_) {
        const auto before=ipc_->dispatch_count();
        const auto sequence=runtime_->events()->sequence();
        const auto received=ipc_->pump();
        if (received && before==ipc_->dispatch_count()) {
            // A hello reply is followed by the command on the same connection.
            // Give that short exchange a bounded owner turn instead of drawing
            // another frame between its two messages. The sequence prevents a lost wake.
            runtime_->events()->wait(sequence,std::chrono::steady_clock::now()+std::chrono::milliseconds{1});
            (void)ipc_->pump();
        }
        if (before!=ipc_->dispatch_count() && !runtime_->stopping()) {
            if (auto refreshed=refresh(); !refreshed) throw std::runtime_error{refreshed.error().message};
        }
    }
#endif
}
#ifdef _WIN32
Result<void> Workspace::start_ipc(std::string_view name) {
    if (ipc_) return std::unexpected(Error{ErrorCode::invalid_state,"IPC is already running"});
    if (!snapshot_) return std::unexpected(Error{ErrorCode::invalid_state,"Open a scene before starting editor IPC"});
    auto server=IpcServer::listen(*runtime_,name);
    if (!server) return std::unexpected(server.error());
    ipc_=std::move(*server);
    return {};
}
void Workspace::close_ipc() { if (ipc_) ipc_->close(std::chrono::milliseconds{1500}); }
#endif
Result<void> Workspace::request_shutdown() {
    if (stopping()) return {};
    auto result=call("runtime.shutdown");
    if (!result) return std::unexpected(result.error());
    return {};
}
Result<Json> Workspace::call(std::string_view method, Json p) {
    auto result=runtime_->dispatch(method,p);
    if (!result) return std::unexpected(result.error());
    return std::move(result->result);
}
Result<Json> Workspace::execute_console(std::string_view text) {
    auto command = parse_console_command(text);
    if (!command) return std::unexpected(command.error());
    return detail::dispatch_console(*command, pending(),
        [this](std::string_view method, const Json& params) { return call(method, params); },
        [this] { return stopping(); }, [this,&command]()->Result<void> {
            if(command->method.starts_with("editor.viewport.")) return {};
            return refresh();
        });
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
    selection_.reset(); draft_.reset(); environment_.reset();
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
        selection_.reset(); draft_.reset(); environment_.reset();
    }
    snapshot_=std::move(*candidate);
    ++snapshot_publication_;
    if(!environment_ || !environment_->modified) {
        auto env=revert_environment(); if(!env) return env;
    }
    manifest_=snapshot_->manifest;
    history_={history->at("undo_count").get<std::size_t>(),history->at("redo_count").get<std::size_t>(),
        history->at("logical_bytes").get<std::size_t>()};
    if (selection_) {
        const auto entities=snapshot_->scene.entities();
        if (std::ranges::none_of(entities,[&](const auto& e) { return e.id==selection_; })) {
            selection_.reset(); draft_.reset();
        } else if (!draft_ || !draft_->modified) return revert();
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
    if (!draft_ || !draft_->modified) return {};
    if(environment_ && environment_->modified) return std::unexpected(pending_error());
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
Result<void> Workspace::revert_environment() {
    if(!snapshot_) { environment_.reset(); return {}; }
    auto query=call("environment.query"); if(!query) return std::unexpected(query.error());
    environment_=EnvironmentDraft{{snapshot_->state.document_id,snapshot_->state.revision},query->at("environment"),false};
    return {};
}
Result<void> Workspace::preset_environment(std::string_view name) {
    if(!environment_) return std::unexpected(Error{ErrorCode::invalid_state,"Open a scene before choosing an environment preset"});
    auto preset=call("environment.preset",{{"name",name}}); if(!preset) return std::unexpected(preset.error());
    environment_->value=preset->at("environment"); environment_->modified=true; return {};
}
Result<void> Workspace::apply_environment() {
    if(!environment_ || !environment_->modified) return {};
    if(draft_ && draft_->modified) return std::unexpected(pending_error());
    auto result=call("environment.set",{{"guard",guard_json(environment_->guard)},{"environment",environment_->value}});
    if(!result) return std::unexpected(result.error());
    environment_->modified=false; return refresh();
}
Result<void> Workspace::save() {
    if (!snapshot_) return std::unexpected(Error{ErrorCode::invalid_state,"Open a project before saving."});
    if(environment_ && environment_->modified && draft_ && draft_->modified) return std::unexpected(pending_error());
    if (auto applied=apply_environment(); !applied) return applied;
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

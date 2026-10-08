#include "WorkbenchUi.hpp"
#include "EditorSupport.hpp"
#include <imgui_internal.h>
#include <imgui_stdlib.h>
#include <algorithm>
#include <unordered_map>

namespace dk::editor::detail {
WorkbenchUi::WorkbenchUi(Workspace& model,bool fixture) : model_(model),input_(fixture),manifest_(take(path_to_utf8(model.manifest()))) {
    log_.push_back("Opened "+manifest_);
    log_.push_back("Viewport: CPU picking / local Gizmo / orbit camera. Edits use guarded commands.");
}
void WorkbenchUi::mark(const char* id) {
    const auto a=ImGui::GetItemRectMin(),b=ImGui::GetItemRectMax();
    controls_[id]={(a.x+b.x)*0.5f,(a.y+b.y)*0.5f};
}
bool WorkbenchUi::report(const char* action,Result<void> result) {
    std::string message=action;
    if (!result) {
        message+=" failed: "+result.error().message;
        for (const auto& context:result.error().context) message+=" / "+context;
        ++errors_;
    }
    else message+=" succeeded";
    if (log_.size()==64) log_.erase(log_.begin());
    log_.push_back(std::move(message)); scroll_log_=true;
    return bool(result);
}
void WorkbenchUi::perform() {
    if (pending_==PendingAction::close) closing_=true;
    else if (pending_==PendingAction::open) {
        auto path=path_from_utf8(manifest_);
        if (!path) report("Open",std::unexpected(path.error()));
        else report("Open",model_.open(*path,true));
    }
    pending_=PendingAction::none;
}
void WorkbenchUi::request(PendingAction action) {
    if (pending_!=PendingAction::none) return;
    pending_=action;
    if (model_.dirty()) popup_=true;
    else perform();
}
void WorkbenchUi::request_close() { input_.cancel(); request(PendingAction::close); }
void WorkbenchUi::toolbar() {
    if (!ImGui::BeginMainMenuBar()) return;
    ImGui::TextUnformatted("DECKER  /  WORKBENCH"); ImGui::Separator();
    ImGui::BeginDisabled(input_.active());
    if (ImGui::Button("Save")) report("Save",model_.save()); mark("save");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(220);
    ImGui::InputText("##manifest",&manifest_); mark("manifest");
    ImGui::SameLine();
    if (ImGui::Button("Open")) request(PendingAction::open); mark("open");
    ImGui::SameLine();
    if (ImGui::Button("Reload")) { manifest_=take(path_to_utf8(model_.manifest())); request(PendingAction::open); } mark("reload");
    ImGui::Separator();
    const auto history=model_.history();
    ImGui::BeginDisabled(model_.pending() || !history.undo_count);
    if (ImGui::Button("Undo")) report("Undo",model_.undo()); mark("undo");
    ImGui::EndDisabled(); ImGui::SameLine();
    ImGui::BeginDisabled(model_.pending() || !history.redo_count);
    if (ImGui::Button("Redo")) report("Redo",model_.redo()); mark("redo");
    ImGui::EndDisabled(); ImGui::Separator();
    if (const auto* s=model_.snapshot()) ImGui::Text("rev %llu  |  %s",
        static_cast<unsigned long long>(s->state.revision),model_.dirty() ? "Unsaved changes" : "Saved");
    ImGui::EndDisabled(); ImGui::EndMainMenuBar();
}
void WorkbenchUi::hierarchy() {
    ImGui::Begin("Hierarchy");
    const auto* snapshot=model_.snapshot();
    if (snapshot) {
        ImGui::Text("%zu entities",snapshot->state.entity_count);
        ImGui::Separator();
        if (model_.pending()) ImGui::TextWrapped("Apply or Revert the Inspector draft before changing selection.");
        using Row=std::pair<const EntityData*,unsigned>;
        std::unordered_map<EntityId,std::vector<const EntityData*>> children;
        for (const auto& e : snapshot->scene.entities()) children[e.parent.value_or(EntityId{})].push_back(&e);
        std::vector<Row> stack,rows;
        const auto append=[&](EntityId parent,unsigned depth) {
            for (auto i=children[parent].rbegin();i!=children[parent].rend();++i) stack.emplace_back(*i,depth);
        };
        append({},0);
        while (!stack.empty()) {
            auto row=stack.back(); stack.pop_back(); rows.push_back(row); append(row.first->id,row.second+1);
        }
        ImGui::BeginDisabled(model_.pending() || input_.active());
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(rows.size()));
        while (clipper.Step()) for (int i=clipper.DisplayStart;i<clipper.DisplayEnd;++i) {
            const auto& [e,depth]=rows[static_cast<std::size_t>(i)];
            ImGui::PushID(e->id.to_string().c_str());
            const float indent=static_cast<float>(std::min(depth,8u))*12;
            if (indent>0) ImGui::Indent(indent);
            const auto label=(children[e->id].empty() ? "  " : "> ")+(e->name.empty() ? "(unnamed)" : e->name);
            if (ImGui::Selectable(label.c_str(),model_.selection()==e->id)) report("Select",model_.select(e->id));
            if (i==0) mark("entity");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s\n%s",e->name.c_str(),e->id.to_string().c_str());
            if (indent>0) ImGui::Unindent(indent);
            ImGui::PopID();
        }
        ImGui::EndDisabled();
    }
    ImGui::End();
}
void WorkbenchUi::inspector() {
    ImGui::Begin("Inspector");
    auto* draft=model_.draft();
    if (!draft) { ImGui::TextWrapped("Select an entity in Hierarchy to inspect or edit it."); ImGui::End(); return; }
    ImGui::TextUnformatted("ENTITY"); ImGui::TextWrapped("%s",draft->entity.id.to_string().c_str());
    ImGui::Separator();
    ImGui::BeginDisabled(input_.active());
    bool modified=false;
    ImGui::SetNextItemWidth(-1);
    modified|=ImGui::InputText("##Name",&draft->entity.name); mark("name");
    ImGui::TextDisabled("Name");
    ImGui::Spacing(); ImGui::TextUnformatted("LOCAL TRANSFORM");
    ImGui::SetNextItemWidth(-1);
    modified|=ImGui::InputScalarN("##Translation",ImGuiDataType_Double,draft->entity.local.translation.data(),3,nullptr,nullptr,"%.4f"); mark("translation");
    const auto translation_min=ImGui::GetItemRectMin(),translation_max=ImGui::GetItemRectMax();
    controls_["translation"]={translation_min.x+(translation_max.x-translation_min.x)/6,(translation_min.y+translation_max.y)*0.5f};
    ImGui::TextDisabled("Translation  X / Y / Z");
    ImGui::SetNextItemWidth(-1);
    modified|=ImGui::InputScalarN("##Rotation",ImGuiDataType_Double,draft->entity.local.rotation.coeffs().data(),4,nullptr,nullptr,"%.4f");
    ImGui::TextDisabled("Quaternion  X / Y / Z / W");
    ImGui::SetNextItemWidth(-1);
    modified|=ImGui::InputScalarN("##Scale",ImGuiDataType_Double,draft->entity.local.scale.data(),3,nullptr,nullptr,"%.4f");
    ImGui::TextDisabled("Scale  X / Y / Z");
    draft->modified|=modified;
    ImGui::Spacing();
    ImGui::BeginDisabled(!model_.pending());
    if (ImGui::Button("Apply")) report("Apply",model_.apply()); mark("apply");
    ImGui::SameLine();
    if (ImGui::Button("Revert")) report("Revert",model_.revert()); mark("revert");
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    // Commands may replace the draft; reacquire before displaying it.
    draft=model_.draft();
    if (draft && draft->modified) ImGui::TextWrapped("Draft not applied. Save also applies this draft.");
    if (draft) {
        ImGui::Separator(); ImGui::TextUnformatted("REFERENCES");
        if (draft->entity.parent) ImGui::TextWrapped("Parent: %s",draft->entity.parent->to_string().c_str());
        else ImGui::TextDisabled("Root entity");
        for (const auto& a : draft->entity.assets) {
            ImGui::TextUnformatted(asset_kind_name(a.kind).data()); ImGui::TextWrapped("%s",a.id.to_string().c_str());
        }
    }
    ImGui::End();
}
void WorkbenchUi::assets() {
    ImGui::Begin("Assets");
    if (const auto* s=model_.snapshot()) {
        ImGui::TextWrapped("%s  /  %zu mapped assets",s->project.description().name.c_str(),s->project.description().assets.size());
        if (ImGui::BeginTable("assets",2,ImGuiTableFlags_RowBg|ImGuiTableFlags_BordersInnerH|ImGuiTableFlags_Resizable)) {
            ImGui::TableSetupColumn("Kind",ImGuiTableColumnFlags_WidthFixed,80); ImGui::TableSetupColumn("Project path");
            ImGui::TableHeadersRow();
            for (const auto& a:s->project.description().assets) {
                ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextUnformatted(asset_kind_name(a.kind).data());
                ImGui::TableNextColumn(); ImGui::TextUnformatted(a.path.c_str());
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s",a.id.to_string().c_str());
            }
            ImGui::EndTable();
        }
    }
    ImGui::End();
}
void WorkbenchUi::console() {
    ImGui::Begin("Console");
    for (const auto& line:log_) ImGui::TextWrapped("%s",line.c_str());
    if (scroll_log_) { ImGui::SetScrollHereY(1.0f); scroll_log_=false; }
    ImGui::End();
}
void WorkbenchUi::confirmation() {
    if (popup_) { ImGui::OpenPopup("Unsaved changes"); popup_=false; }
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(),ImGuiCond_Appearing,{0.5f,0.5f});
    if (ImGui::BeginPopupModal("Unsaved changes",nullptr,ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("The current scene or Inspector draft has unsaved changes.");
        ImGui::TextUnformatted("Save before continuing?");
        if (ImGui::Button("Save and continue") && report("Save",model_.save())) { perform(); ImGui::CloseCurrentPopup(); } mark("confirm_save");
        ImGui::SameLine();
        if (ImGui::Button("Discard")) { perform(); ImGui::CloseCurrentPopup(); } mark("discard");
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) { pending_=PendingAction::none; ImGui::CloseCurrentPopup(); } mark("cancel");
        ImGui::EndPopup();
    }
}
void WorkbenchUi::draw(Viewport& preview,bool srgb) {
    controls_.clear();
    if (ImGui::GetIO().AppFocusLost || ImGui::IsKeyPressed(ImGuiKey_Escape,false)) input_.cancel();
    if (input_.edit() && (!model_.snapshot() || input_.edit()->guard.document_id!=model_.snapshot()->state.document_id ||
        input_.edit()->guard.revision!=model_.snapshot()->state.revision)) input_.cancel();
    toolbar();
    const auto dock=ImGui::DockSpaceOverViewport(0,ImGui::GetMainViewport());
    if (!layout_) {
        ImGui::DockBuilderRemoveNode(dock);
        ImGui::DockBuilderAddNode(dock,ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(dock,ImGui::GetMainViewport()->WorkSize);
        auto center=dock;
        const auto left=ImGui::DockBuilderSplitNode(center,ImGuiDir_Left,0.18f,nullptr,&center);
        const auto right=ImGui::DockBuilderSplitNode(center,ImGuiDir_Right,0.31f,nullptr,&center);
        auto bottom=ImGui::DockBuilderSplitNode(center,ImGuiDir_Down,0.26f,nullptr,&center);
        const auto log=ImGui::DockBuilderSplitNode(bottom,ImGuiDir_Right,0.48f,nullptr,&bottom);
        ImGui::DockBuilderDockWindow("Hierarchy",left); ImGui::DockBuilderDockWindow("Inspector",right);
        ImGui::DockBuilderDockWindow("Viewport",center); ImGui::DockBuilderDockWindow("Assets",bottom);
        ImGui::DockBuilderDockWindow("Console",log); ImGui::DockBuilderFinish(dock);
        layout_=true;
    }
    hierarchy(); inspector();
    const bool visible=ImGui::Begin("Viewport",nullptr,ImGuiWindowFlags_NoScrollWithMouse|ImGuiWindowFlags_NoScrollbar);
    if (!visible) input_.cancel();
    ImGui::TextUnformatted("UNLIT PREVIEW"); ImGui::SameLine();
    ImGui::BeginDisabled(input_.active());
    if (ImGui::SmallButton("Refresh")) preview.retry();
    ImGui::EndDisabled();
    input_.toolbar(model_,preview,controls_);
    if (const auto* s=model_.snapshot(); s && visible) {
        if (preview.texture()) ImGui::Text("rev %llu  /  %zu draws  /  %s",
            static_cast<unsigned long long>(preview.info().revision),preview.info().draw_count,
            preview.current(s->state) ? "Current scene" : "STALE PREVIEW");
        else ImGui::TextUnformatted("Preparing scene...");
        const auto available=ImGui::GetContentRegionAvail();
        if (available.x>=32 && available.y>=32) {
            const auto width=static_cast<std::uint32_t>(std::clamp(available.x,32.0f,1600.0f));
            const auto height=static_cast<std::uint32_t>(std::clamp(available.y,32.0f,1200.0f));
            preview.update(*s,width,height,srgb,input_.camera(),input_.edit(),input_.revision());
            if (preview.texture()) {
                const auto& info=preview.info();
                const float scale=std::min(available.x/static_cast<float>(info.width),available.y/static_cast<float>(info.height));
                ImGui::Image(preview.texture(),{static_cast<float>(info.width)*scale,static_cast<float>(info.height)*scale});
                input_.image(model_,preview,controls_,[&](const char* action,Result<void> result) { return report(action,std::move(result)); });
            } else ImGui::TextWrapped("Preview unavailable. See Console and press Refresh to retry.");
        } else input_.cancel();
        if (preview.error()!=viewport_error_) {
            viewport_error_=preview.error();
            if (!viewport_error_.empty()) report("Viewport",std::unexpected(Error{ErrorCode::internal_error,viewport_error_}));
        }
    }
    ImGui::End();
    assets(); console(); confirmation();
}
}

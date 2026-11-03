#include "WorkbenchUi.hpp"
namespace dk::editor::detail {
void WorkbenchUi::prepare_help(const Viewport& preview) {
    if(!console_catalog_ || console_catalog_->version!=model_.command_catalog_version()) console_catalog_=model_.command_catalog();
    SettingsContext c;
    c.scene=model_.snapshot(); c.selection=bool(model_.selection());
    c.inspector_draft=model_.draft() && model_.draft()->modified;
    c.environment_draft=model_.environment_draft() && model_.environment_draft()->modified;
    c.gizmo=input_.active(); c.navigating=input_.navigating(); c.camera_popup=input_.camera_popup();
    const auto camera=input_.camera(); c.orthographic=camera.projection_mode()==ProjectionMode::orthographic;
    c.automatic_clip=camera.settings().lens.automatic_clip;
    const auto& v=model_.editor_viewport()->settings(); c.path_mode=v.path_trace; c.restir=v.path.restir_di; c.scene_media=v.path.scene_media;
#ifdef DK_EDITOR_PATHTRACING
    c.path_available=preview.path_unavailable().empty(); c.path_reason=preview.path_unavailable();
#endif
    c.small_cloud_view=std::uint64_t{preview.info().width}*preview.info().height<=4096;
    c.composing=console_composing_; c.simulation_gpu=Workspace::gpu_simulation_available();
    const auto state=model_.simulation_state(); c.simulation_run=bool(state.run);
    c.simulation_paused=state.mode==SimulationMode::paused; c.simulation_stopping=state.mode==SimulationMode::stopping;
    if(state.run) {
        c.simulation_fault=bool(state.run->fault);
        c.simulation_view=simulation_visible_ && state.run->cloth && !c.simulation_stopping;
        const auto& task=state.run->task;
        c.simulation_terminal=task && (task->status==SimulationTaskStatus::succeeded || task->status==SimulationTaskStatus::cancelled || task->status==SimulationTaskStatus::failed);
    }
    settings_help_.begin_frame(*console_catalog_,std::move(c),model_.snapshot() ? model_.snapshot()->state.document_id : DocumentId{});
}
void WorkbenchUi::settings_search() {
    if(auto text=settings_help_.draw(console_input_.text().empty(),console_composing_,controls_)) {
        if(auto r=console_input_.insert_template(*text); !r) settings_help_.feedback(r.error().message);
        else ImGui::SetWindowFocus("Console");
    }
}
}

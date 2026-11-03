#include "WorkbenchUi.hpp"
namespace dk::editor::detail {
bool WorkbenchUi::viewport_settings_changed(const EditorViewportSettings& candidate) {
    model_.viewport_interacting(input_.active() || input_.navigating());
    auto& host=*model_.editor_viewport();
    if(candidate==host.settings()) return true;
    auto state=host.query();
    if(!state) return report("Viewport settings",std::unexpected(state.error()));
    auto result=host.set(state->guard,candidate);
    return report("Viewport settings",result ? Result<void>{} : std::unexpected(result.error()));
}
void WorkbenchUi::viewport_action(EditorViewportControl action) {
    model_.viewport_interacting(input_.active() || input_.navigating());
    auto& host=*model_.editor_viewport(); auto state=host.query();
    if(!state) { report("Viewport action",std::unexpected(state.error())); return; }
    auto result=host.control(state->guard,action);
    report("Viewport action",result ? Result<void>{} : std::unexpected(result.error()));
}
void WorkbenchUi::observe_viewport(const Viewport& preview,bool visible) {
    auto& host=*model_.editor_viewport(); auto state=host.query(); if(!state) return;
    EditorViewportPresentation p;
    p.has_image=bool(preview.texture()) && !preview.simulation_run();
    p.current=visible && p.has_image && model_.snapshot() && preview.current(model_.snapshot()->state) &&
        preview.camera_current(input_.camera()) && preview.published_path()==state->settings.path_trace;
    p.document_id=preview.published_document(); p.scene_revision=preview.info().revision;
    p.camera_revision=preview.published_camera().revision(); p.width=preview.info().width; p.height=preview.info().height;
    p.renderer=preview.published_path() ? "pathtrace" : "raster";
    p.error=preview.error(); p.phase=p.current ? "Complete" : "Pending";
#ifdef DK_EDITOR_PATHTRACING
    if(state->settings.path_trace) {
        const auto status=preview.path_status(); p.phase=status.phase;
        p.generation=status.progress.generation; p.target_spp=status.progress.target_spp;
        p.spp=preview.published_path() ? preview.path_samples() : 0;
        if(!status.error.empty()) p.error=status.error;
    }
#endif
    if(!visible) p.phase="Hidden";
    if(!p.error.empty()) { p.phase="Failed"; p.current=false; }
    host.present(state->guard,std::move(p));
}
}

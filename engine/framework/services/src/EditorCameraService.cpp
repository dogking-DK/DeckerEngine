#include <dk/services/EditorViewportService.hpp>
#include <cmath>
namespace dk {
editor::Camera EditorViewportService::effective_camera(const EditorViewportContext& context) const {
    camera_.set_environment(context.scene_lighting && !settings_.path_trace && !settings_.unlit_inspection);
    camera_.set_pathtracing(settings_.path_trace);
    return camera_;
}
editor::Camera EditorViewportService::camera() const {
    if(!closed_) if(auto context=context_()) return effective_camera(*context);
    return camera_;
}
void EditorViewportService::camera_aspect(double aspect) { if(!closed_ && std::isfinite(aspect) && aspect>0) aspect_=aspect; }
Result<EditorCameraState> EditorViewportService::query_camera() const {
    auto state=query(); if(!state) return std::unexpected(state.error());
    return EditorCameraState{std::move(*state),camera(),aspect_};
}
Result<EditorCameraState> EditorViewportService::commit_camera(EditorViewportGuard guard,editor::Camera candidate,bool gui) {
    auto state=query_camera(); if(!state) return state;
    if(guard!=state->viewport.guard) return std::unexpected(Error{ErrorCode::conflict,"Camera guard is stale; query the identified Edit view"});
    auto context=context_(); if(!context) return std::unexpected(context.error());
    const auto& blocked=gui ? context->gui_camera_blocked_reason : context->blocked_reason;
    if(!blocked.empty()) return std::unexpected(Error{ErrorCode::invalid_state,blocked});
    candidate.set_environment(context->scene_lighting && !settings_.path_trace && !settings_.unlit_inspection);
    candidate.set_pathtracing(settings_.path_trace);
    if(auto valid=candidate.set_settings(candidate.settings(),aspect_); !valid) return std::unexpected(valid.error());
    if(candidate.same_session(state->camera)) return state;
    const auto old_projection=state->camera.projection_result(aspect_);
    const bool render_changed=!old_projection || *old_projection!=candidate.projection(aspect_) || state->camera.world().matrix()!=candidate.world().matrix();
    if(revision_==UINT64_MAX || (render_changed && state->camera.revision()==UINT64_MAX))
        return std::unexpected(Error{ErrorCode::invalid_state,"Camera revision exhausted"});
    candidate.revision_=state->camera.revision()+(render_changed ? 1 : 0);
    state->camera=candidate; ++state->viewport.guard.revision; state->viewport.presentation.current=false;
    // Result allocations and all validation precede the non-allocating publication.
    camera_=candidate; ++revision_;
    return state;
}
Result<EditorCameraState> EditorViewportService::reset_camera(EditorViewportGuard guard,bool defaults,bool gui) {
    auto state=query_camera(); if(!state) return state;
    if(defaults) { if(auto r=state->camera.restore_defaults(fixture_,aspect_); !r) return std::unexpected(r.error()); }
    else state->camera.reset(fixture_);
    return commit_camera(guard,state->camera,gui);
}
Result<EditorCameraState> EditorViewportService::frame_camera(EditorViewportGuard guard,EntityId entity,bool gui) {
    auto state=query_camera(); if(!state) return state;
    if(guard!=state->viewport.guard) return std::unexpected(Error{ErrorCode::conflict,"Camera guard is stale"});
    auto context=context_(); if(!context) return std::unexpected(context.error());
    const auto& blocked=gui ? context->gui_camera_blocked_reason : context->blocked_reason;
    if(!blocked.empty()) return std::unexpected(Error{ErrorCode::invalid_state,blocked});
    if(!frame_bounds_) return std::unexpected(Error{ErrorCode::invalid_state,"Edit geometry is not available for framing"});
    auto bounds=frame_bounds_(entity,context->document_id,context->scene_revision); if(!bounds) return std::unexpected(bounds.error());
    if(auto r=state->camera.frame(*bounds,aspect_); !r) return std::unexpected(r.error());
    return commit_camera(guard,state->camera,gui);
}
}

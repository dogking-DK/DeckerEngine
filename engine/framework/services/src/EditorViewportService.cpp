#include <dk/services/EditorViewportService.hpp>
#include <cmath>
#include <limits>

namespace dk {
namespace {
Result<void> validate(const EditorViewportSettings& settings) {
    const auto& p=settings.path;
    const auto range=[](double v,double low,double high){return std::isfinite(v) && v>=low && v<=high;};
    if(!range(settings.raster_exposure,0,1e6) || !p.target || p.bounces<1 || p.bounces>32 ||
       !p.restir_candidates || p.restir_candidates>64 || !range(p.exposure,-16,16) ||
       !range(p.time_seconds,-1e9,1e9) || !range(p.cloud_albedo,0,1) || !range(p.cloud_anisotropy,-.95,.95) ||
       !p.sky.allFinite() || (p.sky.array()<0).any() || (p.sky.array()>1000).any() || !p.surface_only)
        return std::unexpected(Error{ErrorCode::invalid_argument,"Viewport settings outside supported ranges"});
    return {};
}
}
Result<std::shared_ptr<EditorViewportService>> EditorViewportService::create(EditorViewportCapabilities capabilities,Context context) {
    if(!context) return std::unexpected(Error{ErrorCode::invalid_argument,"Viewport context is required"});
    auto id=EditorViewId::generate(); if(!id) return std::unexpected(id.error());
    return std::shared_ptr<EditorViewportService>(new EditorViewportService(*id,std::move(capabilities),std::move(context)));
}
Result<EditorViewportState> EditorViewportService::query() const {
    if(closed_) return std::unexpected(Error{ErrorCode::invalid_state,"Editor viewport is closed"});
    auto context=context_(); if(!context) return std::unexpected(context.error());
    EditorViewportState state{{id_,context->document_id,revision_},settings_,capabilities_,context->blocked_reason,presentation_};
    state.presentation.current &= state.presentation.observed==state.guard &&
        state.presentation.document_id==context->document_id && state.presentation.scene_revision==context->scene_revision;
    return state;
}
Result<EditorViewportState> EditorViewportService::write(EditorViewportGuard guard,const EditorViewportSettings& candidate,bool explicit_path,bool resetting) {
    auto state=query(); if(!state) return state;
    if(guard!=state->guard) return std::unexpected(Error{ErrorCode::conflict,"Viewport guard is stale; query the identified Edit view"});
    if(!state->blocked_reason.empty()) return std::unexpected(Error{ErrorCode::invalid_state,state->blocked_reason});
    if(auto valid=validate(candidate); !valid) return std::unexpected(valid.error());
    if(!capabilities_.pathtracing && (explicit_path || candidate.path_trace || candidate.path!=settings_.path))
        return std::unexpected(Error{ErrorCode::not_supported,capabilities_.unavailable.empty() ? "Path tracing unavailable" : capabilities_.unavailable});
    if(!resetting && candidate.path.reset!=settings_.path.reset)
        return std::unexpected(Error{ErrorCode::invalid_argument,"Use the viewport reset action"});
    if(candidate==settings_) return state;
    if(revision_==std::numeric_limits<std::uint64_t>::max()) return std::unexpected(Error{ErrorCode::invalid_state,"Viewport revision exhausted"});
    state->settings=candidate; ++state->guard.revision; state->presentation.current=false;
    // All validation and owning result construction precede publication. Values cannot allocate.
    settings_=candidate; ++revision_;
    return state;
}
Result<EditorViewportState> EditorViewportService::set(EditorViewportGuard guard,const EditorViewportSettings& settings,bool explicit_path) {
    return write(guard,settings,explicit_path,false);
}
Result<EditorViewportState> EditorViewportService::control(EditorViewportGuard guard,EditorViewportControl action) {
    auto state=query(); if(!state) return state;
    if(guard!=state->guard) return std::unexpected(Error{ErrorCode::conflict,"Viewport guard is stale"});
    if(!settings_.path_trace || !capabilities_.pathtracing)
        return std::unexpected(Error{ErrorCode::invalid_state,"Pause/resume/reset require the Edit path tracing mode"});
    auto candidate=settings_;
    if(action==EditorViewportControl::reset) {
        if(candidate.path.reset==UINT64_MAX) return std::unexpected(Error{ErrorCode::invalid_state,"Reset sequence exhausted"});
        ++candidate.path.reset;
    } else candidate.path.paused=action==EditorViewportControl::pause;
    return write(guard,candidate,true,action==EditorViewportControl::reset);
}
void EditorViewportService::present(EditorViewportGuard observed,EditorViewportPresentation value) {
    auto state=query();
    if(!state || state->guard!=observed) return;
    value.observed=observed;
    if(value.has_image && value.current) value.shown=observed;
    else if(value.has_image) value.shown=presentation_.shown;
    presentation_=std::move(value);
}
void EditorViewportService::close() { closed_=true; context_={}; }
}

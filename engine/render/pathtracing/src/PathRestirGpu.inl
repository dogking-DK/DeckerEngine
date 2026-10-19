// Kept with the renderer's private scene/resource owners; no SDK types in this API.
namespace {
void validate_restir(const PathTraceSettings& path,const RestirDiSettings& r,std::uint64_t pixels,PathTraceSamples samples,bool fixed_work=false) {
    if(r.version!=1 || r.initial_candidates<1 || r.initial_candidates>64 ||
        !r.max_candidate_evaluations || r.max_candidate_evaluations>(1ull<<24) || path.estimator!=PathTraceEstimator::mis)
        throw path_invalid("Invalid GPU initial RIS settings or estimator combination");
    if(pixels*samples.count*r.initial_candidates>r.max_candidate_evaluations ||
        pixels*samples.count*(2ull*path.max_bounces+2)>path.max_rays ||
        (fixed_work && std::uint64_t(samples.count)*r.initial_candidates>PathTraceRenderer::restir_max_candidates_per_invocation))
        throw path_invalid("GPU initial RIS candidate or ray budget exceeded");
}
bool same_restir(const std::optional<RestirDiSettings>& a,const RestirDiSettings& b) {
    return a && a->version==b.version && a->initial_candidates==b.initial_candidates;
}
void restir_memory(const PathGpuState& state,std::uint64_t pixels,std::uint32_t batch_spp) {
    const auto stats=state.budget->stats();
    const auto payload=pixels*(224ull+(batch_spp>1 ? 128ull : 0ull));
    if(payload>stats.limit-stats.current) throw path_invalid("GPU RIS candidate planes exceed available memory budget");
}
}
Result<PathTraceFrame> PathTraceRenderer::submit_restir_rays(SubmissionQueue& queue,const PathTraceGpuScene& scene,
    std::span<const PathTraceRay> rays,const PathTraceSettings& settings,const RestirDiSettings& restir,
    PathTraceSamples samples,std::uint32_t first_pixel,std::stop_token stop) const try {
    ready(state_,queue,stop);
    if(!scene || scene.state_->program.lock()!=state_) throw path_invalid("Path scene belongs to a different renderer");
    const auto& cpu=*PathReferenceAccess::get(scene.source()); check(validate_path_work(cpu,rays.size(),settings,samples));
    validate_restir(settings,restir,rays.size(),samples,true);
    if(rays.size()-1>std::numeric_limits<std::uint32_t>::max()-first_pixel) throw path_invalid("Path pixel counter overflow");
    Vector<Float4> packed(0,memory::Allocator<Float4>{state_->heap});
    for(const auto& ray:rays) {
        check(validate_path_ray(ray)); cancelled(stop);
        const double maximum=std::min(ray.maximum,double(std::numeric_limits<float>::max()));
        if(ray.minimum>maximum) throw path_invalid("Path ray interval exceeds float domain");
        packed.push_back(pack(ray.origin,ray.minimum==0 ? 0.0 : ray.minimum)); packed.push_back(pack(ray.direction,maximum));
    }
    const auto width=std::uint32_t(rays.size()); auto p=parameters(cpu,settings,samples,width,1,true,first_pixel);
    p[32]=integers(restir.initial_candidates,2,0,0);
    auto info=info_for(scene.source(),width,1,settings,samples); info.fixed_rays=true; info.first_pixel=first_pixel; info.restir=restir;
    return PathTraceFrame{submit(*state_,queue,*scene.state_,p,packed,std::move(info),stop)};
} catch(const Error& error) { return std::unexpected(error); }
catch(const std::exception& error) { return std::unexpected(Error{ErrorCode::internal_error,error.what()}); }

Result<PathTraceFrame> PathTraceRenderer::submit_restir_view(SubmissionQueue& queue,const PathTraceGpuScene& scene,
    const RenderView& view,const PathTraceSettings& settings,const RestirDiSettings& restir,PathTraceSamples samples,std::stop_token stop) const try {
    ready(state_,queue,stop);
    if(!scene || scene.state_->program.lock()!=state_) throw path_invalid("Path scene belongs to a different renderer");
    const auto& cpu=*PathReferenceAccess::get(scene.source()); check(validate_path_view(cpu,view));
    const auto& d=view.description(); check(validate_path_work(cpu,std::uint64_t{d.width}*d.height,settings,samples));
    validate_restir(settings,restir,std::uint64_t{d.width}*d.height,samples,true);
    auto p=view_parameters(cpu,view,settings,samples); p[32]=integers(restir.initial_candidates,2,0,0);
    auto info=info_for(scene.source(),d.width,d.height,settings,samples); info.view=d; info.restir=restir;
    const std::array<Float4,2> dummy{};
    return PathTraceFrame{submit(*state_,queue,*scene.state_,p,dummy,std::move(info),stop)};
} catch(const Error& error) { return std::unexpected(error); }
catch(const std::exception& error) { return std::unexpected(Error{ErrorCode::internal_error,error.what()}); }

Result<void> PathTraceRenderer::configure_restir_view(SubmissionQueue& queue,std::uint64_t id,const PathTraceGpuScene& scene,
    const RenderView& view,const PathTraceSettings& settings,const RestirDiSettings& restir,const PathTraceSchedule& schedule) try {
    ready(state_,queue,{});
    if(state_->terminal) throw Error{ErrorCode::invalid_state,"Path session device lost"};
    if(!scene || scene.state_->program.lock()!=state_) throw path_invalid("Path scene belongs to a different renderer");
    if(!schedule.batch_spp || schedule.batch_spp>16) throw path_invalid("Invalid RIS batch size");
    auto validation_schedule=schedule; validation_schedule.batch_spp=1;
    validate_progressive(*PathReferenceAccess::get(scene.source()),view,settings,validation_schedule);
    validate_restir(settings,restir,1,{0,1});
    const auto& d=view.description(); const auto pixels=std::uint64_t{d.width}*d.height;
    if(pixels*schedule.target_spp*(2ull*settings.max_bounces+2)>(1ull<<32)) throw path_invalid("GPU RIS progressive target exceeds ray budget");
    auto* slot=view_slot(*state_,id);
    if(slot) {
        const auto& previous=**slot; const auto& a=previous.view.description();
        if(!previous.medium && same_restir(previous.restir,restir) && previous.scene==scene.state_ && a.width==d.width && a.height==d.height &&
            a.camera_world.matrix()==d.camera_world.matrix() && a.projection==d.projection && same_integrator(previous.settings,settings) &&
            previous.schedule.session==schedule.session && previous.schedule.gesture==schedule.gesture) {
            (*slot)->settings=settings; (*slot)->restir=restir; (*slot)->schedule=schedule; return {};
        }
    } else {
        for(auto& item:state_->views) if(!item) { slot=&item; break; }
        if(!slot) throw Error{ErrorCode::conflict,"Path renderer has two occupied view slots"};
    }
    restir_memory(*state_,pixels,schedule.batch_spp);
    auto candidate=memory::make_shared_in<PathGpuView>(state_->heap,view);
    candidate->id=id; candidate->scene=scene.state_; candidate->settings=settings; candidate->restir=restir; candidate->schedule=schedule;
    candidate->generation=state_->generation+1;
    *slot=std::move(candidate); ++state_->generation; return {};
} catch(const Error& error) { return std::unexpected(error); }
catch(const std::exception& error) { return std::unexpected(Error{ErrorCode::internal_error,error.what()}); }

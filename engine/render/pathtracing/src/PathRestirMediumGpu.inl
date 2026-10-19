// Explicit medium RIS entry points share the renderer, field cache and epoch scheduler.
Result<PathTraceFrame> PathTraceRenderer::submit_restir_medium_rays(SubmissionQueue& queue,const PathTraceGpuScene& scene,
    const PathMediumSnapshot& medium,std::span<const PathTraceRay> rays,const VolumePathSettings& settings,const RestirDiSettings& restir,
    PathTraceSamples samples,std::uint32_t first_pixel,std::stop_token stop) const try {
    ready(state_,queue,stop);
    if(!scene || scene.state_->program.lock()!=state_) throw path_invalid("Path scene belongs to a different renderer");
    validate_medium(scene.source(),medium,settings,samples,rays.size());
    validate_restir(common_settings(settings),restir,rays.size(),samples,true,true);
    if(rays.size()-1>std::numeric_limits<std::uint32_t>::max()-first_pixel) throw path_invalid("Path pixel counter overflow");
    Vector<Float4> packed(0,memory::Allocator<Float4>{state_->heap});
    for(const auto& ray:rays) {
        check(validate_path_ray(ray)); cancelled(stop);
        const double maximum=std::min(ray.maximum,double(std::numeric_limits<float>::max()));
        if(ray.minimum>maximum) throw path_invalid("Path ray interval exceeds float domain");
        packed.push_back(pack(ray.origin,ray.minimum==0 ? 0.0 : ray.minimum)); packed.push_back(pack(ray.direction,maximum));
        packed.push_back(radial_origin(medium,ray.origin));
        for(const auto& phase:cloud_origin(medium,ray.origin)) packed.push_back(phase);
    }
    const auto width=std::uint32_t(rays.size());
    auto p=parameters(*PathReferenceAccess::get(scene.source()),common_settings(settings),samples,width,1,true,first_pixel);
    medium_parameters(p,medium,settings); p[32]=integers(restir.initial_candidates,2,0,0);
    auto info=medium_info(scene.source(),medium,settings,width,1,samples);
    info.fixed_rays=true; info.first_pixel=first_pixel; info.restir=restir; info.sampling_first_epoch=samples.first;
    return PathTraceFrame{submit(*state_,queue,*scene.state_,p,packed,std::move(info),stop,nullptr,nullptr,prepare_cloud(*state_,medium))};
} catch(const Error& error) { return std::unexpected(error); }
catch(const std::exception& error) { return std::unexpected(Error{ErrorCode::internal_error,error.what()}); }

Result<PathTraceFrame> PathTraceRenderer::submit_restir_medium_view(SubmissionQueue& queue,const PathTraceGpuScene& scene,
    const PathMediumSnapshot& medium,const RenderView& view,const VolumePathSettings& settings,const RestirDiSettings& restir,
    PathTraceSamples samples,std::stop_token stop) const try {
    ready(state_,queue,stop);
    if(!scene || scene.state_->program.lock()!=state_) throw path_invalid("Path scene belongs to a different renderer");
    const auto& d=view.description(); const auto pixels=std::uint64_t{d.width}*d.height;
    validate_medium(scene.source(),medium,settings,samples,pixels); validate_restir(common_settings(settings),restir,pixels,samples,true,true);
    const auto& cpu=*PathReferenceAccess::get(scene.source()); check(validate_path_view(cpu,view));
    auto p=view_parameters(cpu,view,common_settings(settings),samples); medium_parameters(p,medium,settings);
    medium_origin(p,medium,{p[6][0],p[6][1],p[6][2]}); p[32]=integers(restir.initial_candidates,2,0,0);
    auto info=medium_info(scene.source(),medium,settings,d.width,d.height,samples); info.view=d; info.restir=restir; info.sampling_first_epoch=samples.first;
    const std::array<Float4,3> dummy{};
    return PathTraceFrame{submit(*state_,queue,*scene.state_,p,dummy,std::move(info),stop,nullptr,nullptr,prepare_cloud(*state_,medium))};
} catch(const Error& error) { return std::unexpected(error); }
catch(const std::exception& error) { return std::unexpected(Error{ErrorCode::internal_error,error.what()}); }

Result<void> PathTraceRenderer::configure_restir_medium_view(SubmissionQueue& queue,std::uint64_t id,const PathTraceGpuScene& scene,
    const PathMediumSnapshot& medium,const RenderView& view,const VolumePathSettings& settings,const RestirDiSettings& restir,const PathTraceSchedule& schedule) try {
    ready(state_,queue,{});
    if(state_->terminal) throw Error{ErrorCode::invalid_state,"Path session device lost"};
    if(!scene || scene.state_->program.lock()!=state_) throw path_invalid("Path scene belongs to a different renderer");
    if(!schedule.target_spp || !schedule.batch_spp || schedule.batch_spp>16 ||
        !schedule.tile_edge || schedule.tile_edge>256 || !std::isfinite(schedule.exposure) || std::abs(schedule.exposure)>32)
        throw path_invalid("Invalid medium RIS target, batch, tile or exposure");
    validate_medium(scene.source(),medium,settings,{0,1},1);
    const auto common=common_settings(settings); validate_restir(common,restir,1,{0,1},false,true);
    const auto& cpu=*PathReferenceAccess::get(scene.source()); validate_gpu_view(queue.device(),cpu,view);
    const auto& d=view.description(); const auto pixels=std::uint64_t{d.width}*d.height;
    auto p=view_parameters(cpu,view,common,{0,1}); medium_parameters(p,medium,settings);
    medium_origin(p,medium,{p[6][0],p[6][1],p[6][2]});
    if(restir.temporal) {
        const Mat4d clip=d.projection*d.camera_world.matrix().inverse();
        for(unsigned row=0;row<4;++row) (void)pack(clip.block<1,3>(row,0).transpose(),clip(row,3));
    }
    auto* slot=view_slot(*state_,id); std::shared_ptr<const PathGpuCompleted> sampling_history; std::uint32_t epoch_origin=0;
    if(slot) {
        const auto& previous=**slot; const auto& a=previous.view.description();
        if(previous.medium && same_restir(previous.restir,restir) && previous.scene==scene.state_ &&
            previous.medium->environment()==medium.environment() && previous.medium->settings()==medium.settings() &&
            previous.medium->field_hash()==medium.field_hash() && a.width==d.width && a.height==d.height && a.projection==d.projection &&
            same_integrator(previous.settings,common) && previous.schedule.session==schedule.session && previous.schedule.gesture==schedule.gesture) {
            if(a.camera_world.matrix()==d.camera_world.matrix()) {
                if(std::uint64_t(previous.epoch_origin)+schedule.target_spp>std::numeric_limits<std::uint32_t>::max())
                    throw path_invalid("Medium RIS epoch counter overflow");
                (*slot)->settings=common; (*slot)->medium_settings=settings; (*slot)->restir=restir; (*slot)->schedule=schedule; return {};
            }
            if(restir.temporal) {
                sampling_history=previous.sampling_history;
                if(sampling_history) epoch_origin=sampling_history->info.sampling_first_epoch+sampling_history->info.samples.count;
            }
        }
    } else {
        for(auto& item:state_->views) if(!item) { slot=&item; break; }
        if(!slot) throw Error{ErrorCode::conflict,"Path renderer has two occupied view slots"};
    }
    if(std::uint64_t(epoch_origin)+schedule.target_spp>std::numeric_limits<std::uint32_t>::max()) throw path_invalid("Medium RIS epoch counter overflow");
    auto candidate=memory::make_shared_in<PathGpuView>(state_->heap,view);
    candidate->id=id; candidate->scene=scene.state_; candidate->medium=medium; candidate->medium_settings=settings;
    candidate->cloud=prepare_cloud(*state_,medium); restir_memory(*state_,pixels,std::min(schedule.batch_spp,schedule.target_spp),restir);
    candidate->settings=common; candidate->restir=restir; candidate->schedule=schedule;
    candidate->sampling_history=std::move(sampling_history); candidate->epoch_origin=epoch_origin; candidate->generation=state_->generation+1;
    if(candidate->cloud) state_->cloud=candidate->cloud;
    *slot=std::move(candidate); ++state_->generation; return {};
} catch(const Error& error) { return std::unexpected(error); }
catch(const std::exception& error) { return std::unexpected(Error{ErrorCode::internal_error,error.what()}); }

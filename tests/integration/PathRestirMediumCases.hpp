#pragma once
namespace {
PathTraceGpuImage ris_medium_image(PathTraceRenderer& r,SubmissionQueue& q,const PathTraceGpuScene& scene,const PathMediumSnapshot& m,
    VolumePathSettings s,unsigned count,const PathTraceRay& ray,const RestirDiSettings& ris={},unsigned first=0,unsigned pixel=0) {
    return complete(q,take(r.submit_restir_medium_rays(q,scene,m,std::span{&ray,1},s,ris,{first,count},pixel)));
}
Vec3d ris_medium_mean(PathTraceRenderer& r,SubmissionQueue& q,const PathTraceGpuScene& scene,const PathMediumSnapshot& m,
    VolumePathSettings s,unsigned count,const PathTraceRay& ray,const RestirDiSettings& ris={}) {
    Vec3d value=Vec3d::Zero();
    for(unsigned first=0;first<count;) {
        const auto chunk=std::min({count-first,8192u,PathTraceRenderer::restir_max_candidates_per_invocation/ris.initial_candidates});
        value+=ris_medium_image(r,q,scene,m,s,chunk,ray,ris,first).linear_rgb()[0]*(double(chunk)/count); first+=chunk;
    }
    return value;
}
void ris_medium_population(PathTraceRenderer& r,SubmissionQueue& q,const PathTraceGpuScene& scene,const PathMediumSnapshot& m,
    const PathTraceRay& ray,VolumePathSettings s,unsigned count=32768,const Vec3d* expected=nullptr,bool classic=false,const RestirDiSettings& ris={}) {
    std::array<Vec3d,8> values,reference;
    for(unsigned seed=0;seed<8;++seed) {
        s.seed=101+7919*seed; values[seed]=ris_medium_mean(r,q,scene,m,s,count,ray,ris);
        reference[seed]=expected ? *expected : (classic ? take(estimate_volume_path_reference(scene.source(),m,ray,s,{0,count})).mean :
            take(estimate_restir_volume_reference(scene.source(),m,ray,s,ris,{0,count})).mean);
    }
    temporal_mean(values,reference);
}
void medium_horizontal(Fixture& f,float height,Vec3f color) {
    f.triangle({-200,height,-200},{0,height,200},{200,height,-200},color);
}
RenderView medium_down_view(Fixture& f,unsigned size=5,bool ortho=false,double height=1,double x=.2) {
    auto d=f.view(f.source(),size).description(); Trsd pose; pose.translation={x,height,.1};
    pose.rotation=Quatd{Eigen::AngleAxisd{-std::numbers::pi/2,Vec3d::UnitX()}}; d.camera_world=take(Transformd::from_trs(pose));
    if(ortho) d.projection=take(orthographic_projection({1,.1,20000},1));
    else { d.projection(2,2)=20000./(.1-20000); d.projection(2,3)=2000./(.1-20000); }
    return take(RenderView::create(f.source(),d));
}
void restir_medium_numeric(memory::ResourceHandle heap,PathTraceRenderer& r,SubmissionQueue& q) {
    Fixture f{heap}; auto s=air_settings(); s.environment_rgb={.3,.5,.7}; auto m=take(PathMediumSnapshot::create({}));
    auto empty=take(r.prepare_scene(q,f.scene()));
    compare_images(ris_medium_image(r,q,empty,m,s,128,down),cloud_image(r,q,empty,m,s,128,down),0);
    f.quad(-8,8,0,{.4f,.6f,.8f}); auto gpu=take(r.prepare_scene(q,f.scene()));
    ris_medium_population(r,q,gpu,m,down,s);
    RestirDiSettings one; one.initial_candidates=1; ris_medium_population(r,q,gpu,m,down,s,65536,nullptr,false,one);
    auto neg=down; neg.minimum=-0.; compare_images(ris_medium_image(r,q,gpu,m,s,128,down),ris_medium_image(r,q,gpu,m,s,128,neg),0);
    for(bool ground:{false,true}) {
        Fixture a{heap}; auto e=air_environment(0,0); e.atmosphere.mie_extinction_per_m={3e-5,1.5e-4,3e-4};
        const Vec3d color=Vec3f{.7f,.3f,.9f}.cast<double>(); e.atmosphere.ground_albedo={color.x(),color.y(),color.z()};
        e.sun.angular_radius_rad=1e-4; e.sun.irradiance={1,.8,.6}; if(!ground) medium_horizontal(a,1000,color.cast<float>());
        check(a.document->set_environment(e)); auto medium=take(PathMediumSnapshot::create(e)); auto scene=take(r.prepare_scene(q,a.scene()));
        s=air_settings(); s.sun=true; const PathTraceRay ray{{0,8000,0},{0,-1,0}}; Vec3d truth;
        for(unsigned c=0;c<3;++c) truth[c]=color[c]*e.sun.irradiance[c]/std::numbers::pi*
            std::exp(-e.atmosphere.mie_extinction_per_m[c]*10000*(2*std::exp(-(ground ? 0. : 1000.)/10000)-std::exp(-.8)-std::exp(-1.)));
        ris_medium_population(r,q,scene,medium,ray,s,65536,&truth);
        ris_medium_population(r,q,scene,medium,ray,s,8192);
        e.sun.direction={0,-1,0}; check(a.document->set_environment(e)); scene=take(r.prepare_scene(q,a.scene())); medium=take(PathMediumSnapshot::create(e));
        close_color(ris_medium_image(r,q,scene,medium,s,128,ray).linear_rgb()[0],Vec3d::Zero(),0);
    }
    std::puts("medium RIS vacuum/M1/RGB survival/ground/mesh/planet shadow/CPU=pass");
}
void restir_medium_lights(memory::ResourceHandle heap,PathTraceRenderer& r,SubmissionQueue& q) {
    Fixture f{heap}; auto e=air_environment(2e-5,1e-5); e.atmosphere.planet_center_m[1]-=100;
    e.sun.direction={0,0,1}; e.sun.angular_radius_rad=.05; e.sun.irradiance={1,2,3}; check(f.document->set_environment(e));
    f.quad(-8,8,0,{.7f,.4f,.2f}); f.quad(-1,1,2,{.2f,.3f,.4f},{3,2,1},true);
    auto m=take(PathMediumSnapshot::create(e)); auto s=air_settings(3); s.rr_start=1; s.sun=true; s.environment_rgb={.3,.4,.5};
    ris_medium_population(r,q,take(r.prepare_scene(q,f.scene())),m,down,s,32768,nullptr,true);
    for(unsigned i:{2u,3u}) f.asset.materials[i].alpha_mode=AlphaMode::mask;
    ris_medium_population(r,q,take(r.prepare_scene(q,f.scene())),m,down,s,32768);
    for(unsigned i:{2u,3u}) f.asset.materials[i].base_color.w()=0;
    for(unsigned i:{0u,1u}) { f.asset.materials[i].metallic=.7f; f.asset.materials[i].roughness=.4f; }
    ris_medium_population(r,q,take(r.prepare_scene(q,ggx_scene(f))),m,down,s,32768);
    // Binary UV weights avoid treating hardware interpolation precision as estimator error.
    for(unsigned i:{0u,1u}) { add_texture(f,i,{.375f,.625f}); f.asset.textures.back().sampler.mag_filter=TextureFilter::linear; }
    for(unsigned i:{2u,3u}) { f.asset.materials[i].alpha_mode=AlphaMode::opaque; f.asset.materials[i].base_color.w()=1; }
    ris_medium_population(r,q,take(r.prepare_scene(q,ggx_scene(f))),m,down,s,32768);
    auto no_bounce=air_settings(0); no_bounce.sun=true; auto gpu=take(r.prepare_scene(q,ggx_scene(f)));
    const PathTraceRay up{{.2,.1,1},{0,0,1}};
    compare_images(ris_medium_image(r,q,gpu,m,no_bounce,1024,up),cloud_image(r,q,gpu,m,no_bounce,1024,up),0);
    std::puts("medium RIS finite sun/environment/area/mask/GGX/textures/secondary RR=pass");
}
void restir_medium_cloud(memory::ResourceHandle heap,PathTraceRenderer& r,SubmissionQueue& q) {
    Fixture f{heap}; auto e=cloud_environment(); e.sun.angular_radius_rad=1e-6; e.atmosphere.ground_albedo={.5,.5,.5};
    check(f.document->set_environment(e)); auto gpu=take(r.prepare_scene(q,f.scene())); CloudFieldCache cache; auto fields=take(cache.prepare(e.clouds));
    PathMediumSettings ms; ms.cloud_albedo=0; auto s=air_settings(); s.sun=true; const PathTraceRay ray{{0,1,0},{0,-1,0}};
    Vec3d old=Vec3d::Zero();
    for(double time:{17.,2017.}) {
        ms.time_seconds=time; auto m=take(PathMediumSnapshot::create(e,ms,fields));
        const auto fine=take(m.integrate({{0,0,0},{0,1,0},100000},{8192,40960})).transmittance;
        close_color(fine,take(m.integrate({{0,0,0},{0,1,0},100000},{4096,40960})).transmittance,1e-4);
        const Vec3d truth=fine*(.5/std::numbers::pi); ris_medium_population(r,q,gpu,m,ray,s,65536,&truth);
        const auto image=ris_medium_image(r,q,gpu,m,s,1024,ray); compare_images(image,ris_medium_image(r,q,gpu,m,s,1024,ray),0);
        require(image.info().medium->medium.time_seconds==time && image.info().restir,"medium RIS provenance absent");
        if(time>17) require((image.linear_rgb()[0]-old).norm()>.01,"wind did not change current shadow"); old=image.linear_rgb()[0];
    }
    ms.cloud_albedo=1; ms.time_seconds=17; e.atmosphere.ground_albedo={1,1,1}; s=air_settings(32); s.rr_start=3; s.environment_rgb=Vec3d::Ones();
    const Vec3d white=Vec3d::Ones();
    for(bool mixed:{false,true}) {
        if(mixed) { e.enabled=true; e.atmosphere.mie_extinction_per_m=e.atmosphere.mie_scattering_per_m={1e-5,1e-5,1e-5}; }
        check(f.document->set_environment(e)); gpu=take(r.prepare_scene(q,f.scene())); auto m=take(PathMediumSnapshot::create(e,ms,fields));
        ris_medium_population(r,q,gpu,m,ray,s,65536,&white);
        const auto cpu=take(estimate_restir_volume_reference(gpu.source(),m,ray,s,{}, {0,8192}));
        require(cpu.restir_statistics.continuation_scatter>0 && cpu.statistics.multiple_scatter_paths>0,"cloud continuation fixture inactive");
        // No geometric receiver on upward rays: real volume vertices remain ordinary transport.
        const PathTraceRay up{{0,2000,0},{0,1,0}};
        compare_images(ris_medium_image(r,q,gpu,m,s,4096,up),cloud_image(r,q,gpu,m,s,4096,up),0);
    }
    std::puts("medium RIS cloud wind/current shadow/multiple scattering/RR/no volume reuse=pass");
}
void restir_medium_schedule(memory::ResourceHandle heap,PathTraceRenderer& r,SubmissionQueue& q) {
    Fixture f{heap}; auto e=cloud_environment(); e.atmosphere.ground_albedo={.5,.4,.3}; check(f.document->set_environment(e));
    CloudFieldCache cache; auto fields=take(cache.prepare(e.clouds)); PathMediumSettings ms; ms.time_seconds=17;
    auto m=take(PathMediumSnapshot::create(e,ms,fields)); auto s=air_settings(4); s.environment_rgb={1,.7,.4}; s.sun=true;
    // Both empty compute and textured triangle RT paths bind a cloud field alongside all RIS planes.
    for(bool geometry:{false,true}) {
        if(geometry) { medium_horizontal(f,1000,{.6f,.4f,.2f}); add_texture(f,0,{.375f,.625f}); }
        auto gpu=take(r.prepare_scene(q,f.scene()));
        for(bool ortho:{false,true}) for(unsigned mode=0;mode<4;++mode) {
            RestirDiSettings ris; if(mode&1) ris.temporal.emplace(); if(mode&2) ris.spatial.emplace();
            if(ris.temporal) ris.temporal->position_tolerance=3000;
            if(ris.spatial) ris.spatial->position_tolerance=3000;
            auto view=medium_down_view(f,5,ortho,2000); PathTraceGpuImage base;
            for(unsigned batch:{1u,7u,16u}) {
                PathTraceSchedule schedule; schedule.target_spp=19; schedule.batch_spp=batch; schedule.tile_edge=batch==1 ? 64 : 3; schedule.reverse_tiles=batch==16;
                check(r.configure_restir_medium_view(q,121,gpu,m,view,s,ris,schedule)); auto image=finish_view(r,q,121);
                const auto& stats=image.info().restir_medium_statistics;
                require(stats.prepared_surfaces==25*19 && stats.applied_surfaces>0 && stats.applied_surfaces<stats.prepared_surfaces,"reservoir conditioned on primary survival");
                if(base) {
                    compare_images(image,base,0); require(stats.applied_surfaces==base.info().restir_medium_statistics.applied_surfaces,"survival depends on schedule");
                } else base=image;
                if(mode&1) require(image.info().temporal_statistics.reused>0,"medium temporal inactive");
                if(mode&2) require(image.info().spatial_statistics.reused>0,"medium spatial inactive");
                check(r.release_view(121));
            }
            if(mode==0) compare_images(base,complete(q,take(r.submit_restir_medium_view(q,gpu,m,view,s,{}, {0,19}))),0);
        }
    }
    std::puts("medium RIS empty/RT/cloud/textured/perspective/ortho/all passes/batch/tile replay=pass");
}
void restir_medium_reuse(memory::ResourceHandle heap,PathTraceRenderer& r,SubmissionQueue& q) {
    Fixture f{heap}; auto e=air_environment(0,0); e.atmosphere.ground_albedo={.7,.3,.9};
    e.atmosphere.mie_extinction_per_m={3e-5,1.5e-4,3e-4}; e.sun.angular_radius_rad=1e-4; e.sun.irradiance={1,.8,.6};
    check(f.document->set_environment(e)); auto gpu=take(r.prepare_scene(q,f.scene())); auto m=take(PathMediumSnapshot::create(e));
    auto view=medium_down_view(f,8,true,8000,0); auto s=air_settings(); s.sun=true; Vec3d expected;
    // Orthographic near plane is .1 m below the eye. The one-metre footprint has negligible curvature.
    for(unsigned c=0;c<3;++c) expected[c]=e.atmosphere.ground_albedo[c]*e.sun.irradiance[c]/std::numbers::pi*
        std::exp(-e.atmosphere.mie_extinction_per_m[c]*10000*(2-std::exp(-7999.9/10000)-std::exp(-1.)));
    for(unsigned mode:{1u,2u,3u}) {
        RestirDiSettings ris; if(mode&1) { ris.temporal.emplace(); ris.temporal->max_history_age=4; }
        if(mode&2) { ris.spatial.emplace(); ris.spatial->max_source_candidates=7; }
        std::array<Vec3d,8> observations,truth; double low_mse=0,high_mse=0;
        for(unsigned chain=0;chain<8;++chain) {
            s.seed=101+7919*chain; PathTraceSchedule schedule; schedule.batch_spp=16; schedule.target_spp=256;
            check(r.configure_restir_medium_view(q,122,gpu,m,view,s,ris,schedule)); auto low=finish_view(r,q,122);
            Vec3d small=Vec3d::Zero(); for(const auto& c:low.linear_rgb()) small+=c/64;
            schedule.target_spp=1024; check(r.configure_restir_medium_view(q,122,gpu,m,view,s,ris,schedule)); auto high=finish_view(r,q,122);
            observations[chain].setZero(); for(const auto& c:high.linear_rgb()) observations[chain]+=c/64;
            truth[chain]=expected; low_mse+=(small-expected).squaredNorm()/24; high_mse+=(observations[chain]-expected).squaredNorm()/24;
            const auto& stats=high.info().restir_medium_statistics;
            require(stats.prepared_surfaces==64*1024 && stats.applied_surfaces<stats.prepared_surfaces,"survival filtered reused histories");
            if(mode&1) require(high.info().temporal_statistics.expired>0 && high.info().temporal_statistics.max_age<=4,"medium ancestry unbounded");
            if(mode&2) require(high.info().spatial_statistics.reused>0 && high.info().spatial_statistics.current_visibility_rays<=stats.applied_surfaces,"medium current visibility not bounded");
            check(r.release_view(122));
        }
        temporal_mean(observations,truth); std::printf("medium reuse mode=%u N MSE=%.9g 4N MSE=%.9g\n",mode,low_mse,high_mse);
        require(high_mse<low_mse,"medium independent-chain convergence failed");
    }
    std::puts("medium RIS temporal/spatial/joint RGB survival independent chains and finite history=pass");
}
void restir_medium_state(memory::ResourceHandle heap,PathTraceRenderer& r,SubmissionQueue& q) {
    Fixture f{heap}; auto e=cloud_environment(); e.atmosphere.ground_albedo={.6,.4,.2}; medium_horizontal(f,1000,{.6f,.4f,.2f});
    check(f.document->set_environment(e)); auto gpu=take(r.prepare_scene(q,f.scene())); CloudFieldCache cache; auto fields=take(cache.prepare(e.clouds));
    PathMediumSettings ms; ms.time_seconds=17; auto m=take(PathMediumSnapshot::create(e,ms,fields));
    auto view=medium_down_view(f,4,true,2000); auto s=air_settings(4); s.environment_rgb=Vec3d::Ones();
    auto ris=spatial_settings(true); PathTraceSchedule schedule; schedule.target_spp=3; schedule.batch_spp=2;
    check(r.configure_restir_medium_view(q,123,gpu,m,view,s,ris,schedule)); auto original=finish_view(r,q,123); auto held=r.completed_frame(123);
    const auto generation=take(r.status(123)).generation; auto changed=medium_down_view(f,4,true,2000,.23);
    check(r.configure_restir_medium_view(q,123,gpu,m,changed,s,ris,schedule)); require(!r.completed_frame(123),"camera kept radiance");
    auto moved=finish_view(r,q,123); require(moved.info().sampling_first_epoch==3 && moved.info().temporal_history_epoch==2 && moved.info().temporal_statistics.reused>0,"medium camera history not reprojected");
    const auto moved_generation=take(r.status(123)).generation;
    schedule.exposure=2; schedule.paused=true; check(r.configure_restir_medium_view(q,123,gpu,m,changed,s,ris,schedule));
    require(take(r.status(123)).generation==moved_generation && !take(r.submit_batch(q,123)),"display change reset medium history");
    schedule.paused=false; schedule.target_spp=5; check(r.configure_restir_medium_view(q,123,gpu,m,changed,s,ris,schedule));
    require(finish_view(r,q,123).info().sampling_first_epoch==3,"stationary camera reset epochs");
    auto owned=r.memory_stats(q).owned.current; ms.time_seconds=18; auto wind=take(PathMediumSnapshot::create(e,ms,fields));
    check(r.configure_restir_medium_view(q,123,gpu,wind,changed,s,ris,schedule)); require(!r.completed_frame(123),"wind retained radiance");
    require(r.memory_stats(q).owned.current<=owned,"wind duplicated cloud field");
    auto refreshed=finish_view(r,q,123); require(refreshed.info().sampling_first_epoch==0 && !refreshed.info().temporal_history_epoch,"wind retained sampling history");
    check(r.configure_restir_medium_view(q,124,gpu,wind,changed,s,ris,schedule)); compare_images(finish_view(r,q,124),refreshed,0);
    require(!r.configure_restir_medium_view(q,125,gpu,wind,changed,s,ris,schedule),"third medium RIS view admitted"); check(r.release_view(124));
    // Rebinding illumination changes resets histories but reuses geometry and field storage.
    e.sun.irradiance={2,3,4}; check(f.document->set_environment(e)); const auto submitted=q.stats().submitted;
    auto rebound=take(r.prepare_scene(q,take(gpu.source().rebind(heap,f.source())))); require(q.stats().submitted==submitted,"light edit rebuilt AS");
    auto light=take(PathMediumSnapshot::create(e,ms,fields)); auto light_view=medium_down_view(f,4,true,2000,.23);
    check(r.configure_restir_medium_view(q,123,rebound,light,light_view,s,ris,schedule)); require(finish_view(r,q,123).info().sampling_first_epoch==0,"light edit retained epochs");
    // Algorithm switches are physical sampling identity changes even with identical medium settings.
    check(r.configure_medium_view(q,123,rebound,light,light_view,s,schedule)); require(!r.completed_frame(123),"classic medium retained RIS result");
    require(!finish_view(r,q,123).info().restir,"classic medium incorrectly reports RIS");
    check(r.configure_restir_medium_view(q,123,rebound,light,light_view,s,ris,schedule)); require(!r.completed_frame(123),"RIS retained classic result"); finish_view(r,q,123);
    for(unsigned change=0;change<9;++change) {
        check(r.configure_restir_medium_view(q,123,rebound,light,light_view,s,ris,schedule)); finish_view(r,q,123);
        auto path=s; auto config=ris; auto scheduling=schedule; auto d=light_view.description(); auto medium_settings=ms;
        if(change==0) d.projection(0,0)*=1.1;
        if(change==1) d.width=5;
        if(change==2) ++path.seed;
        if(change==3) path.environment_rgb*=.5;
        if(change==4) ++config.initial_candidates;
        if(change==5) ++config.temporal->max_history_age;
        if(change==6) ++scheduling.session;
        if(change==7) ++scheduling.gesture;
        if(change==8) medium_settings.cloud_albedo=.5;
        auto input=take(PathMediumSnapshot::create(e,medium_settings,fields)); auto lens=take(RenderView::create(f.source(),d));
        check(r.configure_restir_medium_view(q,123,rebound,input,lens,path,config,scheduling));
        require(!r.completed_frame(123),"physical or sampling edit retained medium radiance");
        auto reset=finish_view(r,q,123);
        require(reset.info().sampling_first_epoch==0 && !reset.info().temporal_history_epoch,"physical or sampling edit retained medium history");
    }
    // Work limits affect future batches, without changing accepted sampling identity.
    check(r.configure_restir_medium_view(q,123,rebound,light,light_view,s,ris,schedule)); finish_view(r,q,123);
    const auto budget_generation=take(r.status(123)).generation;
    auto relaxed=s; relaxed.max_tracking_steps/=2; relaxed.max_steps_per_path/=2; relaxed.max_rays/=2;
    auto less_work=ris; less_work.max_candidate_evaluations/=2;
    check(r.configure_restir_medium_view(q,123,rebound,light,light_view,relaxed,less_work,schedule));
    require(take(r.status(123)).generation==budget_generation && r.completed_frame(123),"work limits reset medium sampling identity");
    schedule.target_spp=7; check(r.configure_restir_medium_view(q,123,rebound,light,light_view,s,ris,schedule)); take(r.submit_batch(q,123)); check(r.reset(123));
    require(!collect(r,q) && !r.completed_frame(123),"late medium candidate published after reset");
    require(generation!=take(r.status(123)).generation,"generation never advanced");
    compare_images(complete(q,take(held.read_linear(q))),original,0); check(r.release_view(123));
    // Replacing field content keeps the BLAS/TLAS while old image/field owners remain alive.
    check(r.configure_restir_medium_view(q,123,rebound,light,light_view,s,ris,schedule)); finish_view(r,q,123);
    ++e.clouds.seed; check(f.document->set_environment(e)); const auto before_field=q.stats().submitted;
    auto replacement=take(r.prepare_scene(q,take(rebound.source().rebind(heap,f.source()))));
    require(q.stats().submitted==before_field,"cloud field edit rebuilt AS");
    auto new_medium=take(PathMediumSnapshot::create(e,ms,take(cache.prepare(e.clouds))));
    require(new_medium.field_hash()!=light.field_hash(),"field replacement fixture unchanged");
    check(r.configure_restir_medium_view(q,123,replacement,new_medium,medium_down_view(f,4,true,2000),s,ris,schedule));
    require(!r.completed_frame(123),"field replacement retained radiance");
    auto field_image=finish_view(r,q,123);
    require(field_image.info().medium->field_hash==new_medium.field_hash() && field_image.info().sampling_first_epoch==0,"field replacement provenance/history mismatch");
    compare_images(complete(q,take(held.read_linear(q))),original,0); check(r.release_view(123));
    std::puts("medium RIS pose/display/wind/field/physical/sampling/algorithm/reset/two-view/AS reuse/old owner=pass");
}
void medium_phase(PathTraceRenderer& r,SubmissionQueue& q,unsigned id,unsigned phase) {
    for(unsigned i=0;i<1000;++i) {
        if(take(r.status(id)).sampling_pass==phase) return;
        take(r.submit_batch(q,id)); collect(r,q);
    }
    throw std::runtime_error("medium RIS phase did not advance");
}
void restir_medium_failure(memory::ResourceHandle heap,SubmissionQueue& q) {
    auto r=take(PathTraceRenderer::create(heap,q,DK_PATH_SHADER_DIR,{},1ull<<20)); Fixture f{heap}; auto e=cloud_environment();
    check(f.document->set_environment(e)); auto gpu=take(r.prepare_scene(q,f.scene())); CloudFieldCache cache; auto fields=take(cache.prepare(e.clouds));
    auto m=take(PathMediumSnapshot::create(e,{},fields)); auto view=medium_down_view(f,4,true,2000); auto s=air_settings(4); s.environment_rgb=Vec3d::Ones();
    auto ris=spatial_settings(true); PathTraceSchedule schedule; schedule.target_spp=2; schedule.batch_spp=2;
    check(r.configure_restir_medium_view(q,126,gpu,m,view,s,ris,schedule)); auto old=finish_view(r,q,126); auto held=r.completed_frame(126);
    schedule.target_spp=4;
    for(unsigned phase:{0u,3u,4u,1u}) {
        check(r.configure_restir_medium_view(q,126,gpu,m,view,s,ris,schedule)); medium_phase(r,q,126,phase);
        fail_submit=true; require(!r.submit_batch(q,126),"injected medium RIS pass submit succeeded");
        require(take(r.status(126)).completed_spp==2,"failed phase published");
        compare_images(complete(q,take(r.completed_frame(126).read_linear(q))),old,0);
    }
    for(bool per_path:{false,true}) {
        auto low=s; if(per_path) low.max_steps_per_path=1; else low.max_tracking_steps=1;
        check(r.configure_restir_medium_view(q,126,gpu,m,view,low,ris,schedule)); medium_phase(r,q,126,1);
        take(r.submit_batch(q,126)); q.device().logical_device().waitIdle(); auto error=r.poll(q);
        require(!error && error.error().context==std::vector<std::string>{"render.pathtracing.tracking_budget"},"medium RIS tracking failure not explicit");
        require(take(r.status(126)).completed_spp==2,"tracking failure published history");
    }
    check(r.configure_restir_medium_view(q,126,gpu,m,view,s,ris,schedule)); medium_phase(r,q,126,4);
    std::stop_source stop; take(r.submit_batch(q,126,stop.get_token())); stop.request_stop(); require(!collect(r,q),"cancel published candidate");
    require(finish_view(r,q,126).info().samples.count==4,"medium RIS recovery lost samples");
    const auto generation=take(r.status(126)).generation,submitted=q.stats().submitted,before=r.memory_stats(q).owned.current;
    auto invalid=s; invalid.estimator=PathTraceEstimator::bsdf_only;
    require(!r.configure_restir_medium_view(q,126,gpu,m,view,invalid,ris,schedule),"BSDF-only medium RIS admitted");
    RestirDiSettings budget=ris; budget.max_candidate_evaluations=18; // I + temporal 2 + spatial 9 = 19.
    require(!r.configure_restir_medium_view(q,126,gpu,m,view,s,budget,schedule),"medium RIS undersized candidate budget admitted");
    require(!r.submit_restir_medium_view(q,gpu,m,view,s,ris,{0,1}),"fixed medium history admitted");
    require(!r.configure_restir_medium_view(q,126,gpu,m,medium_down_view(f,64,true,2000),s,ris,schedule),"medium RIS planes exceeded memory");
    auto huge=e; huge.clouds.base_resolution=128; check(f.document->set_environment(huge)); auto large=take(r.prepare_scene(q,f.scene()));
    auto big=take(PathMediumSnapshot::create(huge,{},take(cache.prepare(huge.clouds))));
    require(!r.configure_restir_medium_view(q,126,large,big,medium_down_view(f,4,true,2000),s,ris,schedule),"cloud upload bypassed shared memory limit");
    require(take(r.status(126)).generation==generation && q.stats().submitted==submitted && r.memory_stats(q).owned.current==before,"rejected medium RIS mutated accepted state");
    compare_images(complete(q,take(held.read_linear(q))),old,0); check(r.release_view(126));
    // Held complete owners, ping-pong and spatial output all count toward the same limit.
    auto bounded=medium_down_view(f,32,true,2000); // Use a view with the original scene identity/revision.
    check(f.document->set_environment(e)); bounded=medium_down_view(f,32,true,2000);
    gpu=take(r.prepare_scene(q,f.scene())); schedule.target_spp=2;
    check(r.configure_restir_medium_view(q,126,gpu,m,bounded,s,ris,schedule)); advance_view(r,q,126); auto a=r.completed_frame(126);
    check(r.reset(126)); advance_view(r,q,126); auto b=r.completed_frame(126);
    check(r.reset(126)); advance_view(r,q,126); auto current=r.completed_frame(126).linear_buffer();
    schedule.target_spp=4; check(r.configure_restir_medium_view(q,126,gpu,m,bounded,s,ris,schedule));
    require(!r.submit_batch(q,126) && r.completed_frame(126).linear_buffer()==current,"held medium owners bypassed budget");
    a={}; b={}; advance_view(r,q,126); check(r.release_view(126));
    require(r.memory_stats(q).owned.peak<=r.memory_stats(q).owned.limit,"medium RIS peak exceeded quota");
    std::puts("medium RIS phase failures/tracking/cancel/invalid/work/memory/held owners/recovery=pass");
}
}

#pragma once
namespace {
PathMediumSnapshot voxel_medium(const EnvironmentDescription& e,CloudFieldCache& cache,double time=17) {
    PathMediumSettings ms; ms.time_seconds=time;
    return take(PathMediumSnapshot::create(e,ms,e.clouds.enabled ? take(cache.prepare(e.clouds)) : std::shared_ptr<const CloudFieldData>{}));
}
void voxel_medium_numeric(memory::ResourceHandle heap,PathTraceRenderer& r,SubmissionQueue& q) {
    path_voxel_test::Fixture f{heap}; const std::array cells{assets::VoxelBlock{{-1,0,0},1,1},assets::VoxelBlock{{0,0,0},1,2}}; f.grid(cells);
    f.quad(-2,2,2,{0,0,0},{2,3,4}); CloudFieldCache cache;
    // Different fixed rays exercise the six-float4 medium upload/readback stride.
    const std::array rays{PathTraceRay{{-.3,.6,1.5},{0,0,-1}},PathTraceRay{{.3,.6,1.5},{0,0,-1}},PathTraceRay{{.3,.6,1.5},{0,0,1}}};
    for(unsigned mode=0;mode<5;++mode) {
        auto e=path_voxel_test::medium_environment(mode); check(f.document->set_environment(e)); const auto m=voxel_medium(e,cache);
        auto s=air_settings(3); s.environment_rgb={.3,.5,.7}; s.seed=23011; s.sun=mode>=3;
        if(mode==4) { f.voxel.voxel->palette[0].metallic=.6f; f.voxel.voxel->palette[0].roughness=.4f; s.rr_start=2; }
        const auto model=mode==4 ? PathTraceMaterialModel::metallic_roughness : PathTraceMaterialModel::lambert;
        const auto cpu=f.scene(model),oracle=f.oracle(model); const auto gpu=take(r.prepare_scene(q,cpu));
        for(bool ris:{false,true}) {
            auto image=complete(q,ris ? take(r.submit_restir_medium_rays(q,gpu,m,rays,s,{}, {0,2048})) : take(r.submit_medium_rays(q,gpu,m,rays,s,{0,2048})));
            for(unsigned i=0;i<rays.size();++i) {
                VolumePathEstimate expected=ris ? static_cast<VolumePathEstimate>(take(estimate_restir_volume_reference(oracle,m,rays[i],s,{}, {0,2048},i))) : take(estimate_volume_path_reference(oracle,m,rays[i],s,{0,2048},i));
                close_color(image.linear_rgb()[i],expected.mean,.008); close_color(image.second_moment()[i],expected.second_moment,.05);
                if(mode<2) {
                    const auto hit=image.primary_hits()[i]; require(hit && hit->kind==(i<2 ? PathGeometryKind::voxel : PathGeometryKind::triangle),"medium typed diagnostic missing");
                    close_color(hit->position,take(cpu.nearest(rays[i]))->position,2e-6);
                }
            }
        }
    }
    // Both directions of mixed shadow occlusion, with a finite solar cone and air.
    for(bool receiver:{false,true}) {
        path_voxel_test::Fixture shadow{heap}; const std::array block{assets::VoxelBlock{{0,0,0},1,1}};
        Trsd thin; thin.scale={1,1,.001}; shadow.grid(block,take(Transformd::from_trs(thin)));
        shadow.quad(-2,2,receiver ? .5f : -.5f,{.7f,.6f,.5f}); auto e=path_voxel_test::medium_environment(2);
        e.atmosphere.mie_scattering_per_m={0,0,0}; check(shadow.document->set_environment(e)); const auto m=voxel_medium(e,cache);
        auto s=air_settings(); s.sun=true; const PathTraceRay ray{{.3,.6,receiver ? .2 : -.2},{0,0,-1}};
        auto gpu=take(r.prepare_scene(q,shadow.scene()));
        close_color(cloud_image(r,q,gpu,m,s,128,ray).linear_rgb()[0],Vec3d::Zero(),1e-7);
        close_color(ris_medium_image(r,q,gpu,m,s,128,ray).linear_rgb()[0],Vec3d::Zero(),1e-7);
    }
    std::puts("voxel medium vacuum/zero/RGB air/cloud/GGX/RR/area/typed diagnostics/mixed shadows oracle=pass");
}
void voxel_medium_schedule(memory::ResourceHandle heap,PathTraceRenderer& r,SubmissionQueue& q) {
    path_voxel_test::Fixture f{heap}; std::vector<assets::VoxelBlock> blocks;
    for(int x=-2;x<3;++x) blocks.push_back({{x,0,0},1,1+x%2*(x>0 ? 1 : -1)}); f.grid(blocks);
    f.quad(-4,4,-1,{.2f,.6f,.4f}); auto e=path_voxel_test::medium_environment(4); check(f.document->set_environment(e));
    CloudFieldCache cache; const auto m=voxel_medium(e,cache); const auto gpu=take(r.prepare_scene(q,f.scene()));
    auto s=air_settings(3); s.environment_rgb={.7,.8,.9};
    for(bool ortho:{false,true}) for(unsigned mode=0;mode<5;++mode) {
        auto view=voxel_signal_view(f,ortho,.5,4);
        if(ortho) {auto lens=view.description(); lens.projection=take(orthographic_projection({.4,.1,10},1)); view=take(RenderView::create(f.source(),lens));}
        const auto ris=voxel_ris(mode==4 ? 0 : mode); PathTraceGpuImage base;
        for(unsigned batch:{1u,5u}) {
            std::printf("medium schedule ortho=%d mode=%u batch=%u\n",ortho,mode,batch); std::fflush(stdout);
            PathTraceSchedule schedule; schedule.target_spp=9; schedule.batch_spp=batch; schedule.tile_edge=batch==1 ? 64 : 2; schedule.reverse_tiles=batch!=1;
            if(mode==4) check(r.configure_medium_view(q,241,gpu,m,view,s,schedule));
            else check(r.configure_restir_medium_view(q,241,gpu,m,view,s,ris,schedule));
            auto image=finish_view(r,q,241); if(base) {compare_images(image,base,0); if(mode!=4) compare_restir_statistics(image.info(),base.info());} else base=image;
            if(mode!=4) {
                auto stats=image.info().restir_medium_statistics; require(stats.prepared_surfaces>0 && stats.applied_surfaces>0 && stats.applied_surfaces<stats.prepared_surfaces,"medium conditional view inactive");
                if(mode&1) require(image.info().temporal_statistics.reused>0,"voxel medium temporal inactive");
                if(mode&2) require(image.info().spatial_statistics.reused>0,"voxel medium spatial inactive");
            }
            // Every reported voxel position must lie on its own cell face, including medium jitter.
            for(const auto& hit:image.primary_hits()) if(hit && hit->kind==PathGeometryKind::voxel) {
                const auto axis=hit->face/2; const double plane=hit->cell[axis]+(hit->face%2 ? 1 : 0);
                require(std::abs(hit->position[axis]-plane)<2e-5,"medium view diagnostic position off voxel face");
            }
            check(r.release_view(241));
        }
        if(mode==4) compare_images(complete(q,take(r.submit_medium_view(q,gpu,m,view,s,{0,9}))),base,1e-6);
        if(mode==0) compare_images(complete(q,take(r.submit_restir_medium_view(q,gpu,m,view,s,{}, {0,9}))),base,1e-6);
    }
    std::puts("voxel medium Classic/four DI perspective/ortho/tile/batch/reverse/fixed-view/typed position=pass");
}
void voxel_medium_reuse(memory::ResourceHandle heap,PathTraceRenderer& r,SubmissionQueue& q) {
    path_voxel_test::Fixture f{heap}; const std::array cube{assets::VoxelBlock{{0,0,0},1,1}}; f.grid(cube);
    auto e=path_voxel_test::medium_environment(3); e.clouds.extinction_per_m=.1; e.sun.direction={0,1,0}; e.sun.angular_radius_rad=1e-4;
    check(f.document->set_environment(e)); CloudFieldCache cache; PathMediumSettings ms; ms.time_seconds=17; ms.cloud_albedo=0;
    const auto m=take(PathMediumSnapshot::create(e,ms,take(cache.prepare(e.clouds)))); const auto cpu=f.scene(); const auto gpu=take(r.prepare_scene(q,cpu));
    auto d=f.view(4).description(); Trsd camera; camera.translation={.5,2,.5}; camera.rotation=Eigen::AngleAxisd(-std::numbers::pi/2,Vec3d::UnitX());
    d.camera_world=take(Transformd::from_trs(camera)); d.projection=take(orthographic_projection({.3,.1,10},1)); const auto view=take(RenderView::create(f.source(),d));
    Vec3d truth=Vec3d::Zero();
    // The tiny finite sun cone converges to this independently integrated primary * shadow T.
    // The narrow orthographic footprint avoids cell edges; cloud fields vary on >=10 m scales.
    for(unsigned pixel=0;pixel<16;++pixel) {
        const auto ray=take(path_trace_camera_ray(view,pixel%4,pixel/4,{.5,.5})); const auto hit=take(cpu.nearest(ray)); require(bool(hit),"cloud receiver fixture missed");
        const auto primary=take(m.integrate({ray.origin,ray.direction,hit->distance},{2048,40960})).transmittance;
        const auto shadow=take(m.integrate({hit->position,Vec3d::UnitY(),100},{2048,40960})).transmittance;
        const auto coarse=take(m.integrate({hit->position,Vec3d::UnitY(),100},{1024,40960})).transmittance; close_color(shadow,coarse,1e-5);
        require(primary.maxCoeff()<.99 && shadow.maxCoeff()<.9 && shadow.minCoeff()>.05,"cloud survival/shadow fixture inactive");
        truth+=primary.cwiseProduct(shadow).cwiseProduct(Vec3f{.6f,.4f,.2f}.cast<double>()).cwiseProduct(Vec3d{1,2,3})/(16*std::numbers::pi);
    }
    for(unsigned mode=0;mode<4;++mode) {
        std::array<Vec3d,4> observations; double low_mse=0,high_mse=0; const auto ris=voxel_ris(mode);
        for(unsigned chain=0;chain<4;++chain) {
            auto settings=air_settings(); settings.sun=true; settings.seed=101+7919*chain;
            PathTraceSchedule schedule; schedule.target_spp=128; schedule.batch_spp=16;
            check(r.configure_restir_medium_view(q,243,gpu,m,view,settings,ris,schedule)); const auto low=finish_view(r,q,243); Vec3d low_mean=Vec3d::Zero();
            for(const auto& color:low.linear_rgb()) low_mean+=color/16; low_mse+=(low_mean-truth).squaredNorm();
            schedule.target_spp=512; check(r.configure_restir_medium_view(q,243,gpu,m,view,settings,ris,schedule)); const auto high=finish_view(r,q,243);
            observations[chain]=Vec3d::Zero(); for(const auto& color:high.linear_rgb()) observations[chain]+=color/16;
            high_mse+=(observations[chain]-truth).squaredNorm(); const auto stats=high.info().restir_medium_statistics;
            require(stats.prepared_surfaces==16*512 && stats.applied_surfaces>0 && stats.applied_surfaces<stats.prepared_surfaces,"cloud RIS history conditioned on absorption");
            if(mode&1) require(high.info().temporal_statistics.reused>0 && high.info().temporal_statistics.max_age<=4,"cloud temporal inactive/unbounded");
            if(mode&2) require(high.info().spatial_statistics.reused>0 && high.info().spatial_statistics.current_visibility_rays>0,"cloud spatial/shadow inactive");
            check(r.release_view(243));
        }
        Vec3d mean=Vec3d::Zero(),variance=Vec3d::Zero(); for(const auto& value:observations) mean+=value/4;
        for(const auto& value:observations) variance+=(value-mean).cwiseAbs2()/12;
        std::printf("voxel cloud DI mode=%u mean=(%g %g %g) truth=(%g %g %g) MSE N=%g 4N=%g\n",mode,mean.x(),mean.y(),mean.z(),truth.x(),truth.y(),truth.z(),low_mse,high_mse);
        for(unsigned c=0;c<3;++c) {
            require(std::abs(mean[c]-truth[c])<=.02*truth[c]+1e-4,"voxel cloud mean exceeds 2% integrated reference");
            require(std::abs(mean[c]-truth[c])<=5*std::sqrt(variance[c])+1e-4,"voxel cloud mean exceeds 5 chain standard errors");
        }
        require(high_mse<low_mse,"voxel cloud chain failed to converge");
    }
    std::puts("voxel cloud four DI/independent integrated survival and shadow/chain means/convergence=pass");
}

void voxel_medium_state(memory::ResourceHandle heap,PathTraceRenderer& r,SubmissionQueue& q) {
    path_voxel_test::Fixture f{heap}; const std::array cube{assets::VoxelBlock{{0,0,0},1,1}}; f.grid(cube);
    auto e=path_voxel_test::medium_environment(4); check(f.document->set_environment(e)); CloudFieldCache cache;
    auto m=voxel_medium(e,cache); auto gpu=take(r.prepare_scene(q,f.scene())); auto view=f.view(4); auto s=air_settings(3); s.environment_rgb={.7,.8,.9};
    const auto ris=voxel_ris(3); PathTraceSchedule schedule; schedule.target_spp=3; schedule.batch_spp=2;
    check(r.configure_restir_medium_view(q,242,gpu,m,view,s,ris,schedule)); const auto old=finish_view(r,q,242); auto held=r.completed_frame(242);
    schedule.target_spp=5;
    for(unsigned phase:{0u,3u,4u,1u}) {
        check(r.configure_restir_medium_view(q,242,gpu,m,view,s,ris,schedule)); medium_phase(r,q,242,phase);
        fail_submit=true; require(!r.submit_batch(q,242),"voxel medium failed phase accepted");
        require(!fail_submit && take(r.status(242)).completed_spp==3,"failed phase published");
        compare_images(complete(q,take(r.completed_frame(242).read_linear(q))),old,0);
    }
    auto low=s; low.max_tracking_steps=1;
    check(r.configure_restir_medium_view(q,242,gpu,m,view,low,ris,schedule)); medium_phase(r,q,242,1);
    take(r.submit_batch(q,242)); q.device().logical_device().waitIdle(); auto error=r.poll(q);
    require(!error && error.error().context==std::vector<std::string>{"render.pathtracing.tracking_budget"},"voxel tracking budget failure missing");
    require(take(r.status(242)).completed_spp==3,"tracking failure published");
    check(r.configure_restir_medium_view(q,242,gpu,m,view,s,ris,schedule)); medium_phase(r,q,242,4);
    std::stop_source stop; take(r.submit_batch(q,242,stop.get_token())); stop.request_stop(); require(!collect(r,q),"cancel published");
    finish_view(r,q,242); auto d=view.description(); Trsd camera; camera.translation={.51,.5,2}; d.camera_world=take(Transformd::from_trs(camera)); view=take(RenderView::create(f.source(),d));
    check(r.configure_restir_medium_view(q,242,gpu,m,view,s,ris,schedule)); auto moved=finish_view(r,q,242);
    require(moved.info().sampling_first_epoch==5 && moved.info().temporal_statistics.reused>0,"voxel medium camera history missing");
    for(const auto& hit:moved.primary_hits()) if(hit && hit->kind==PathGeometryKind::voxel) require(std::abs(hit->position.z()-1)<2e-5,"moved medium diagnostic epoch wrong");
    for(unsigned change=0;change<6;++change) {
        if(change==0) f.voxel.voxel->palette[0].base_color={.1f,.7f,.4f};
        if(change==1) {const std::array shifted{assets::VoxelBlock{{1,0,0},1,2}}; f.grid(shifted);}
        if(change==2) {Trsd pose; pose.translation={-1,0,0}; f.transform(pose);}
        if(change==3) m=voxel_medium(e,cache,18);
        if(change==4) {e.atmosphere.mie_extinction_per_m={.4,.6,.8}; check(f.document->set_environment(e)); m=voxel_medium(e,cache,18);}
        if(change==5) {++e.clouds.seed; check(f.document->set_environment(e)); m=voxel_medium(e,cache,18);}
        gpu=take(r.prepare_scene(q,f.scene())); view=f.view(4);
        check(r.configure_restir_medium_view(q,242,gpu,m,view,s,ris,schedule)); require(!r.completed_frame(242),"physical edit retained medium radiance");
        auto reset=finish_view(r,q,242); require(reset.info().sampling_first_epoch==0 && !reset.info().temporal_history_epoch,"physical edit retained history");
    }
    auto invalid=schedule; invalid.signals.enabled=true; const auto generation=take(r.status(242)).generation;
    require(!r.configure_restir_medium_view(q,242,gpu,m,view,s,ris,invalid),"Scene media signals opened");
    require(take(r.status(242)).generation==generation && r.completed_frame(242),"rejected signal mode changed state");
    schedule.target_spp=7; check(r.configure_restir_medium_view(q,242,gpu,m,view,s,ris,schedule)); take(r.submit_batch(q,242)); check(r.reset(242));
    require(!collect(r,q) && !r.completed_frame(242),"late voxel media work published");
    compare_images(complete(q,take(held.read_linear(q))),old,0); check(r.release_view(242));
    std::puts("voxel medium phase/tracking/cancel/late/owner/camera/palette/grid/transform/air/wind/field/signals=pass");
}
}

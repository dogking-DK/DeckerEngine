#pragma once
#include "../RestirSpatialOracle.hpp"
namespace {
RestirDiSettings spatial_settings(bool temporal=false) {
    RestirDiSettings r; r.spatial.emplace(); r.spatial->position_tolerance=4;
    if(temporal) { r.temporal.emplace(); r.temporal->position_tolerance=2; }
    return r;
}
void spatial_numeric(memory::ResourceHandle heap,PathTraceRenderer& r,SubmissionQueue& q) {
    Fixture f{heap}; f.quad(-8,8,0,{.6f,.4f,.2f}); auto gpu=take(r.prepare_scene(q,f.scene()));
    auto view=temporal_view(f,5,false,.3,0,.2); auto s=settings(); s.environment_rgb=Vec3d::Ones();
    auto ris=spatial_settings(); ris.spatial->max_source_candidates=3;
    PathTraceSchedule schedule; schedule.target_spp=64; schedule.batch_spp=16;
    check(r.configure_restir_view(q,101,gpu,view,s,ris,schedule)); auto actual=finish_view(r,q,101);
    const auto reference=restir_spatial_oracle::render(gpu.source(),view,s,ris,64);
    for(unsigned i=0;i<25;++i) close_color(actual.linear_rgb()[i],reference[i],1e-5);
    require(actual.info().spatial_statistics.reused>0 && actual.info().spatial_statistics.max_candidates<=15,"spatial CPU comparison inactive");
    check(r.release_view(101));
    // A whole correlated image/history chain is one observation, not 25 independent pixels.
    const Vec3d color=Vec3f{.6f,.4f,.2f}.cast<double>(); std::array<Vec3d,8> large,truth;
    double small_mse=0,large_mse=0; ris=spatial_settings(true); ris.temporal->max_history_age=4;
    ris.spatial->max_source_candidates=7;
    for(unsigned chain=0;chain<8;++chain) {
        s.seed=101+7919*chain; schedule.target_spp=256;
        check(r.configure_restir_view(q,101,gpu,view,s,ris,schedule)); auto a=finish_view(r,q,101);
        Vec3d low=Vec3d::Zero(); for(const auto& c:a.linear_rgb()) low+=c/25;
        schedule.target_spp=1024; check(r.configure_restir_view(q,101,gpu,view,s,ris,schedule)); auto b=finish_view(r,q,101);
        large[chain]=Vec3d::Zero(); for(const auto& c:b.linear_rgb()) large[chain]+=c/25;
        const auto& stats=b.info().spatial_statistics;
        require(stats.max_age<=4 && stats.max_candidates<=35 && b.info().temporal_statistics.expired>0,"spatial ancestry/M limits bypassed");
        require(stats.current_visibility_rays<=25*1024 && stats.current_visibility_rays>25000,"current visibility is not bounded/fresh");
        truth[chain]=color; small_mse+=(low-color).squaredNorm()/24; large_mse+=(large[chain]-color).squaredNorm()/24;
        check(r.release_view(101));
    }
    temporal_mean(large,truth); std::printf("spatial chains N=256 MSE=%.9g 4N=1024 MSE=%.9g\n",small_mse,large_mse);
    require(large_mse<small_mse,"spatial finite history convergence failed");
    std::puts("spatial independent CPU image oracle and spatiotemporal chain furnace=pass");
}
void spatial_schedule(memory::ResourceHandle heap,PathTraceRenderer& r,SubmissionQueue& q) {
    Fixture f{heap}; f.quad(-.4f,.4f,0,{.6f,.4f,.2f}); f.quad(-.2f,.2f,2,{0,0,0},{3,2,1},true);
    for(unsigned i:{0u,1u}) { add_texture(f,i,{.375f,.625f}); f.asset.textures.back().sampler.mag_filter=TextureFilter::linear; }
    auto gpu=take(r.prepare_scene(q,ggx_scene(f))); auto s=settings(3); s.environment_rgb={.2,.3,.4};
    for(bool temporal:{false,true}) for(bool ortho:{false,true}) {
        auto ris=spatial_settings(temporal); ris.spatial->neighbor_count=8; ris.spatial->radius=1;
        auto view=temporal_view(f,5,ortho); PathTraceGpuImage baseline;
        for(unsigned batch:{1u,7u,16u}) {
            PathTraceSchedule schedule; schedule.target_spp=19; schedule.batch_spp=batch; schedule.tile_edge=batch==1 ? 64 : 3; schedule.reverse_tiles=batch==16;
            check(r.configure_restir_view(q,102,gpu,view,s,ris,schedule)); auto actual=finish_view(r,q,102);
            const auto& stats=actual.info().spatial_statistics;
            require(stats.reused>0 && stats.rejected>0 && stats.current_visibility_rays<=25*19,"spatial schedule fixture inactive");
            if(baseline) {
                compare_images(actual,baseline,0);
                compare_restir_statistics(actual.info(),baseline.info());
            } else baseline=actual;
            check(r.release_view(102));
        }
    }
    Fixture empty{heap}; auto sky=take(r.prepare_scene(q,empty.scene())); auto ris=spatial_settings(true);
    PathTraceSchedule schedule; schedule.target_spp=3; schedule.batch_spp=3;
    check(r.configure_restir_view(q,102,sky,temporal_view(empty,3),s,ris,schedule)); auto image=finish_view(r,q,102);
    for(const auto& c:image.linear_rgb()) close_color(c,s.environment_rgb,1e-7);
    require(image.info().spatial_statistics.current_visibility_rays==0 && image.info().spatial_statistics.reused==0,"empty spatial plane has work");
    check(r.release_view(102));
    Fixture black{heap}; black.quad(-4,4,0,{0,0,0}); auto zero=take(r.prepare_scene(q,black.scene()));
    ris.spatial->neighbor_count=8; ris.spatial->radius=1;
    check(r.configure_restir_view(q,102,zero,temporal_view(black,3),s,ris,schedule)); auto dark=finish_view(r,q,102);
    for(const auto& c:dark.linear_rgb()) close_color(c,Vec3d::Zero(),0);
    require(dark.info().spatial_statistics.reused>0 && dark.info().spatial_statistics.max_candidates==0 &&
        dark.info().spatial_statistics.current_visibility_rays==0,"black Lambert generated candidates or shadows");
    check(r.release_view(102));
    // Nonblack receiver generates candidates, but a light below it has zero target everywhere.
    Fixture unsupported{heap}; unsupported.quad(-4,4,0,{.6f,.4f,.2f}); unsupported.quad(-1,1,-2,{0,0,0},{3,2,1});
    auto below=take(r.prepare_scene(q,unsupported.scene())); auto no_environment=s; no_environment.environment_rgb.setZero();
    check(r.configure_restir_view(q,102,below,temporal_view(unsupported,3),no_environment,ris,schedule)); auto all_zero=finish_view(r,q,102);
    for(const auto& c:all_zero.linear_rgb()) close_color(c,Vec3d::Zero(),0);
    require(all_zero.info().spatial_statistics.reused>0 && all_zero.info().spatial_statistics.max_candidates>8 &&
        all_zero.info().spatial_statistics.current_visibility_rays==0,"zero-weight spatial candidates lost M or traced shadows");
    check(r.release_view(102));
    std::puts("spatial isolated/fused texture perspective orthographic reverse tile batch=pass");
}
void spatial_state(memory::ResourceHandle heap,PathTraceRenderer& r,SubmissionQueue& q) {
    Fixture f{heap}; f.quad(-8,8,0,{.6f,.4f,.2f}); auto gpu=take(r.prepare_scene(q,f.scene())); auto view=temporal_view(f,3);
    auto s=settings(); s.environment_rgb=Vec3d::Ones(); auto ris=spatial_settings(true);
    PathTraceSchedule schedule; schedule.target_spp=4; schedule.batch_spp=2;
    check(r.configure_restir_view(q,103,gpu,view,s,ris,schedule)); auto original=finish_view(r,q,103); auto held=r.completed_frame(103);
    require(!r.submit_restir_view(q,gpu,view,s,ris) && !r.submit_restir_rays(q,gpu,std::span{&down,1},s,ris),"single-pass spatial accepted");
    auto only_spatial=ris; only_spatial.temporal.reset();
    require(!r.submit_restir_view(q,gpu,view,s,only_spatial) && !r.submit_restir_rays(q,gpu,std::span{&down,1},s,only_spatial),"history-free spatial-only accepted");
    auto generation=take(r.status(103)).generation;
    for(unsigned k=0;k<10;++k) {
        auto invalid=ris; auto& p=*invalid.spatial;
        if(k==0) p.neighbor_count=0; if(k==1) p.neighbor_count=9; if(k==2) p.radius=0; if(k==3) p.radius=33;
        if(k==4) p.max_source_candidates=4097; if(k==5) p.position_tolerance=NAN; if(k==6) p.normal_cosine=-1.1;
        if(k==7) p.version=2; if(k==8) invalid.max_candidate_evaluations=18;
        if(k==9) p.position_tolerance=std::numeric_limits<double>::denorm_min();
        require(!r.configure_restir_view(q,103,gpu,view,s,invalid,schedule),"invalid spatial settings accepted");
        require(take(r.status(103)).generation==generation && r.completed_frame(103).linear_buffer()==held.linear_buffer(),"invalid spatial changed owner");
    }
    schedule.target_spp=6; check(r.configure_restir_view(q,103,gpu,view,s,ris,schedule));
    for(unsigned phase:{0u,3u,4u,1u}) {
        if(phase) do { take(r.submit_batch(q,103)); require(!collect(r,q),"failure setup published"); } while(take(r.status(103)).sampling_pass!=phase);
        fail_submit=true; require(!r.submit_batch(q,103),"spatial phase submission not failed");
        require(take(r.status(103)).completed_spp==4 && r.completed_frame(103).linear_buffer()==held.linear_buffer(),"failed spatial phase published");
        if(phase) do { take(r.submit_batch(q,103)); require(!collect(r,q),"cancel setup published"); } while(take(r.status(103)).sampling_pass!=phase);
        take(r.submit_batch(q,103)); check(r.cancel(103)); collect(r,q);
        require(take(r.status(103)).completed_spp==4 && r.completed_frame(103).linear_buffer()==held.linear_buffer(),"cancelled spatial phase published");
    }
    // Abort after the first private epoch has produced and swapped its spatial output.
    for(unsigned i=0;i<4;++i) { take(r.submit_batch(q,103)); require(!collect(r,q),"partial batch published"); }
    require(take(r.status(103)).sampling_epoch==5,"spatial second private epoch missing");
    std::stop_source stop; take(r.submit_batch(q,103,stop.get_token())); stop.request_stop(); collect(r,q);
    auto replay=finish_view(r,q,103);
    check(r.configure_restir_view(q,104,gpu,view,s,ris,schedule)); compare_images(replay,finish_view(r,q,104),0); check(r.release_view(104));
    auto moved=temporal_view(f,3,false,.23,.025); schedule.target_spp=2;
    check(r.configure_restir_view(q,103,gpu,moved,s,ris,schedule)); auto image=finish_view(r,q,103);
    require(image.info().sampling_first_epoch==6 && image.info().temporal_statistics.reused>0,"spatial final history lost on camera move");
    ++ris.spatial->radius; check(r.configure_restir_view(q,103,gpu,moved,s,ris,schedule));
    require(!r.completed_frame(103) && finish_view(r,q,103).info().sampling_first_epoch==0,"spatial config did not reset");
    compare_images(complete(q,take(held.read_linear(q))),original,0); check(r.release_view(103));
    std::puts("spatial validation private epochs phase failures cancellation camera reset and held owner=pass");
}
void spatial_support(memory::ResourceHandle heap,PathTraceRenderer& r,SubmissionQueue& q,bool edges) {
    Fixture f{heap};
    if(edges) {
        // Adjacent materials and geometric normals; a small raised black surface creates a silhouette.
        f.triangle({-1,-1,0},{0,-1,0},{0,1,0},{.7f,.2f,.1f});
        f.triangle({-1,-1,0},{0,1,0},{-1,1,0},{.7f,.2f,.1f});
        f.triangle({0,-1,0},{1,-1,.5f},{1,1,.5f},{.1f,.6f,.8f});
        f.triangle({0,-1,0},{1,1,.5f},{0,1,0},{.1f,.6f,.8f});
        for(unsigned i:{2u,3u}) { f.asset.materials[i].metallic=.7f; f.asset.materials[i].roughness=.4f; }
        f.quad(-.35f,-.1f,.4f,{0,0,0});
        f.quad(-.5f,.5f,2,{0,0,0},{3,2,1},true);
    } else {
        f.quad(-4,4,0,{.6f,.4f,.2f});
        f.triangle({0,-1,.4f},{0,1,.4f},{0,0,2},{0,0,0},{4,3,2});
        f.triangle({.15f,-.4f,.2f},{.15f,.4f,.2f},{.15f,0,.8f},{0,0,0},{0,0,0},true);
        f.asset.materials.back().alpha_mode=AlphaMode::mask;
    }
    auto gpu=take(r.prepare_scene(q,edges ? ggx_scene(f) : f.scene()));
    auto d=temporal_view(f,3,true,0,0,edges ? 2.4 : 1).description();
    Trsd camera; camera.translation={0,edges ? 0 : -.6,1}; d.camera_world=take(Transformd::from_trs(camera));
    auto view=take(RenderView::create(f.source(),d)); auto s=settings(); s.environment_rgb={.2,.3,.4};
    auto ris=spatial_settings(!edges); ris.spatial->neighbor_count=8; ris.spatial->radius=1; ris.spatial->normal_cosine=-1;
    ris.spatial->max_source_candidates=8;
    if(ris.temporal) { ris.temporal->max_history_candidates=4; ris.temporal->max_history_age=4; }
    std::array<std::array<Vec3d,8>,9> observations,reference; std::uint64_t corrections=0,reused=0,visibility=0;
    for(unsigned chain=0;chain<8;++chain) {
        s.seed=101+7919*chain; PathTraceSchedule schedule; schedule.target_spp=4096; schedule.batch_spp=16;
        check(r.configure_restir_view(q,105,gpu,view,s,ris,schedule)); auto image=finish_view(r,q,105);
        const auto& stats=image.info().spatial_statistics; corrections+=stats.support_corrected; reused+=stats.reused; visibility+=stats.current_visibility_rays;
        require(stats.current_visibility_rays<=9*4096,"spatial correction ray budget exceeded");
        auto cpu=take(render_path_reference(heap,gpu.source(),view,s,{0,65536}));
        for(unsigned pixel=0;pixel<9;++pixel) { observations[pixel][chain]=image.linear_rgb()[pixel]; reference[pixel][chain]=cpu.linear_rgb()[pixel]; }
        check(r.release_view(105));
    }
    for(unsigned pixel=0;pixel<9;++pixel) { std::printf("spatial %s pixel=%u\n",edges ? "edges" : "support",pixel); temporal_mean(observations[pixel],reference[pixel]); }
    require(reused>1000 && corrections>0 && visibility>0,"spatial boundary/support fixture inactive");
    std::printf("spatial corrections=%llu reused=%llu fresh_visibility=%llu CPU classic MIS per-pixel mean=pass\n",
        static_cast<unsigned long long>(corrections),static_cast<unsigned long long>(reused),static_cast<unsigned long long>(visibility));
}
void spatial_budget(memory::ResourceHandle heap,SubmissionQueue& q) {
    Fixture f{heap}; f.quad(-8,8,0,{.6f,.4f,.2f});
    auto r=take(PathTraceRenderer::create(heap,q,DK_PATH_SHADER_DIR,{},1ull<<20)); auto gpu=take(r.prepare_scene(q,f.scene()));
    auto s=settings(); s.environment_rgb=Vec3d::Ones(); auto ris=spatial_settings(true);
    PathTraceSchedule schedule; schedule.target_spp=2; schedule.batch_spp=2;
    require(!r.configure_restir_view(q,106,gpu,temporal_view(f,64),s,ris,schedule),"independent spatial planes bypassed budget");
    auto view=temporal_view(f,32);
    check(r.configure_restir_view(q,106,gpu,view,s,ris,schedule)); advance_view(r,q,106); auto a=r.completed_frame(106);
    check(r.reset(106)); advance_view(r,q,106); auto b=r.completed_frame(106);
    check(r.reset(106)); advance_view(r,q,106); auto before=r.completed_frame(106).linear_buffer();
    schedule.target_spp=4; check(r.configure_restir_view(q,106,gpu,view,s,ris,schedule));
    require(!r.submit_batch(q,106) && r.completed_frame(106).linear_buffer()==before,"spatial held-owner rejection published");
    a={}; b={}; advance_view(r,q,106); check(r.release_view(106));
    // The spatial intermediate is required even for a one-epoch tail, but no temporal ping-pong is needed.
    schedule.target_spp=1; schedule.batch_spp=16; auto fits=temporal_view(f,60);
    check(r.configure_restir_view(q,106,gpu,fits,s,ris,schedule)); advance_view(r,q,106); check(r.release_view(106));
    require(!r.configure_restir_view(q,106,gpu,temporal_view(f,64),s,ris,schedule),"single-epoch spatial plane missing from preflight");
    // Exact minimum candidate budget permits bounded one-pixel tiles, not a zero-size tile/division.
    ris.max_candidate_evaluations=19; schedule.target_spp=2; schedule.batch_spp=2; view=temporal_view(f,2);
    check(r.configure_restir_view(q,106,gpu,view,s,ris,schedule)); auto image=finish_view(r,q,106);
    require(image.info().samples.count==2,"minimum spatial work budget failed"); check(r.release_view(106));
    std::puts("spatial full/tail payload held owners release recovery and minimum tile work=pass");
}
}

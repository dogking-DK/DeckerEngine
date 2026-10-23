#pragma once
namespace {
Vec3d temporal_furnace_oracle(std::uint64_t seed,unsigned count,const RestirDiSettings& settings,const Vec3d& color) {
    double oldZ=0,oldW=0; unsigned oldM=0,age=0; Vec3d sum=Vec3d::Zero();
    const double brightness=color.dot(Vec3d{.2126,.7152,.0722})/std::numbers::pi;
    for(unsigned epoch=0;epoch<count;++epoch) {
        RisReservoir fresh; double z=0;
        for(unsigned candidate=0;candidate<settings.initial_candidates;++candidate) {
            const double v=std::max(0.,1-2*restir_reference_sample(seed,0,epoch,candidate,RestirRandomPurpose::light_u));
            if(take(fresh.stream(brightness*v,1/(4*std::numbers::pi),
                restir_reference_sample(seed,0,epoch,candidate,RestirRandomPurpose::replacement)))) z=v;
        }
        unsigned M=fresh.candidates(); double W=fresh.normalization();
        if(oldM && age<settings.temporal->max_history_age) {
            const auto m=std::min(oldM,settings.temporal->max_history_candidates);
            const std::array<RisReuseSource,2> sources{{{brightness*z,W,M},{brightness*oldZ,oldW,m}}};
            const std::array<std::uint8_t,4> support{std::uint8_t(z>0),std::uint8_t(oldZ>0),std::uint8_t(z>0),std::uint8_t(oldZ>0)};
            auto merged=take(resample_ris_sources(sources,support,std::array{0.,restir_reference_sample(seed,0,epoch,0,RestirRandomPurpose::temporal_replacement)}));
            if(merged.selected==1) z=oldZ;
            W=merged.normalization; M=unsigned(merged.candidates); ++age;
        } else age=0;
        oldZ=z; oldW=W; oldM=M; sum+=color*(z*W/std::numbers::pi);
    }
    return sum/count;
}
void temporal_mean(const std::array<Vec3d,8>& observations,const std::array<Vec3d,8>& reference) {
    Vec3d a=Vec3d::Zero(),b=Vec3d::Zero(); for(unsigned i=0;i<8;++i) { a+=observations[i]/8; b+=reference[i]/8; }
    for(unsigned c=0;c<3;++c) {
        double variance=0; for(unsigned i=0;i<8;++i) variance+=std::pow(observations[i][c]-a[c],2)+std::pow(reference[i][c]-b[c],2);
        const double error=std::abs(a[c]-b[c]),se=std::sqrt(variance/56);
        std::printf("temporal RGB[%u] GPU=%.9g reference=%.9g SE=%.9g\n",c,a[c],b[c],se);
        require(error<=.02*std::abs(b[c])+1e-4 && error<=5*se+1e-4,"temporal independent chain mean differs");
    }
}
RestirDiSettings temporal_settings() { RestirDiSettings r; r.temporal.emplace(); return r; }
RenderView temporal_view(Fixture& f,unsigned size=5,bool ortho=false,double x=.2,double angle=0,double extent=1) {
    auto d=f.view(f.source(),size).description(); Trsd camera; camera.translation={x,.1,1};
    camera.rotation=Quatd{Eigen::AngleAxisd{angle,Vec3d::UnitY()}}; d.camera_world=take(Transformd::from_trs(camera));
    if(ortho) d.projection=take(orthographic_projection({extent,.1,10},1));
    else { d.projection(0,0)=2/extent; d.projection(1,1)=-2/extent; }
    return take(RenderView::create(f.source(),d));
}
void temporal_numeric(memory::ResourceHandle heap,PathTraceRenderer& r,SubmissionQueue& q) {
    Fixture f{heap}; f.quad(-8,8,0,{.6f,.4f,.2f}); const Vec3d color=Vec3f{.6f,.4f,.2f}.cast<double>();
    auto gpu=take(r.prepare_scene(q,f.scene())); auto view=temporal_view(f,1,false,.3,0,.05);
    auto s=settings(); s.environment_rgb=Vec3d::Ones(); auto ris=temporal_settings();
    std::array<Vec3d,8> small,large,reference; double mse_small=0,mse_large=0;
    for(unsigned chain=0;chain<8;++chain) {
        s.seed=101+7919*chain; PathTraceSchedule schedule; schedule.target_spp=256; schedule.batch_spp=16;
        check(r.configure_restir_view(q,94,gpu,view,s,ris,schedule)); auto short_image=finish_view(r,q,94); small[chain]=short_image.linear_rgb()[0];
        const auto& short_stats=short_image.info().temporal_statistics;
        std::printf("temporal chain=%u reused=%llu rejected=%llu expired=%llu\n",chain,
            static_cast<unsigned long long>(short_stats.reused),static_cast<unsigned long long>(short_stats.rejected),static_cast<unsigned long long>(short_stats.expired));
        close_color(small[chain],temporal_furnace_oracle(s.seed,256,ris,color),1e-5);
        schedule.target_spp=1024; check(r.configure_restir_view(q,94,gpu,view,s,ris,schedule));
        auto image=finish_view(r,q,94); large[chain]=image.linear_rgb()[0];
        close_color(large[chain],temporal_furnace_oracle(s.seed,1024,ris,color),1e-5);
        const auto& stats=image.info().temporal_statistics;
        require(stats.reused>800 && stats.expired>100 && stats.max_age==8 && stats.max_candidates<=72,"long chain did not refresh or limit M");
        reference[chain]=color; mse_small+=(small[chain]-color).squaredNorm()/24; mse_large+=(large[chain]-color).squaredNorm()/24;
        check(r.release_view(94));
    }
    temporal_mean(large,reference); std::printf("temporal independent 8 chains N=256 MSE=%.9g 4N=1024 MSE=%.9g\n",mse_small,mse_large);
    require(mse_large<mse_small,"finite history did not converge over longer independent chains");
    std::puts("temporal long chains match CPU merge oracle and analytic furnace=pass");
}
void temporal_materials(memory::ResourceHandle heap,PathTraceRenderer& r,SubmissionQueue& q) {
    Fixture f{heap}; f.quad(-8,8,0,{.7f,.4f,.2f}); f.quad(-1,1,2,{0,0,0},{3,2,1},true);
    for(unsigned i:{0u,1u}) { f.asset.materials[i].metallic=.7f; f.asset.materials[i].roughness=.4f; }
    auto gpu=take(r.prepare_scene(q,ggx_scene(f))); auto s=settings(); s.environment_rgb={.2,.3,.4};
    auto ris=temporal_settings(); ris.temporal->position_tolerance=.5;
    auto before=temporal_view(f,1,false,.3,0,1e-5),moved=temporal_view(f,1,false,.3,.15,1e-5);
    const auto ray=take(path_trace_camera_ray(moved,0,0,{.5,.5}));
    std::array<Vec3d,8> observations,reference;
    for(unsigned chain=0;chain<8;++chain) {
        s.seed=101+7919*chain; PathTraceSchedule schedule; schedule.target_spp=64; schedule.batch_spp=16;
        check(r.configure_restir_view(q,95,gpu,before,s,ris,schedule)); finish_view(r,q,95);
        schedule.target_spp=1024; check(r.configure_restir_view(q,95,gpu,moved,s,ris,schedule));
        auto image=finish_view(r,q,95); observations[chain]=image.linear_rgb()[0];
        require(image.info().sampling_first_epoch==64 && image.info().temporal_statistics.reused>800,"GGX moved chain lost reuse");
        reference[chain]=take(estimate_restir_reference(gpu.source(),ray,s,{}, {0,65536})).mean;
        check(r.release_view(95));
    }
    temporal_mean(observations,reference);
    std::puts("temporal GGX view change current visibility CPU initial oracle mean=pass");
}
void temporal_schedule(memory::ResourceHandle heap,PathTraceRenderer& r,SubmissionQueue& q) {
    Fixture f{heap}; f.quad(-8,8,0,{.6f,.4f,.2f}); f.quad(-1,1,2,{0,0,0},{3,2,1},true);
    auto gpu=take(r.prepare_scene(q,ggx_scene(f))); auto s=settings(3); s.environment_rgb={.2,.3,.4};
    auto ris=temporal_settings(); ris.temporal->position_tolerance=1; ris.temporal->max_history_age=3; ris.temporal->max_history_candidates=9;
    for(bool ortho:{false,true}) {
        auto view=temporal_view(f,5,ortho); PathTraceGpuImage baseline;
        for(unsigned batch:{1u,7u,16u}) {
            PathTraceSchedule schedule; schedule.target_spp=19; schedule.batch_spp=batch; schedule.tile_edge=batch==1 ? 64 : 3; schedule.reverse_tiles=batch==16;
            check(r.configure_restir_view(q,91,gpu,view,s,ris,schedule)); auto actual=finish_view(r,q,91);
            const auto& stats=actual.info().temporal_statistics;
            require(stats.reused>0 && stats.expired>0 && stats.max_age==3 && stats.max_candidates<=17,"temporal limits/reuse missing");
            require(actual.info().sampling_first_epoch==0 && !actual.info().temporal_history_epoch,"fresh view has history origin");
            if(baseline) { compare_images(actual,baseline,0); compare_restir_statistics(actual.info(),baseline.info()); }
            else baseline=actual;
            check(r.release_view(91));
        }
    }
    Fixture empty{heap}; auto sky=take(r.prepare_scene(q,empty.scene())); auto view=temporal_view(empty,3);
    PathTraceSchedule schedule; schedule.target_spp=5; schedule.batch_spp=3;
    check(r.configure_restir_view(q,91,sky,view,s,ris,schedule)); auto actual=finish_view(r,q,91);
    for(const auto& rgb:actual.linear_rgb()) close_color(rgb,s.environment_rgb,1e-7);
    require(actual.info().temporal_statistics.reused==0,"empty scene reused surfaces"); check(r.release_view(91));
    Fixture textured{heap}; textured.quad(-8,8,0,{.8f,.7f,.6f});
    for(unsigned i:{0u,1u}) { add_texture(textured,i,{.375f,.625f}); textured.asset.textures.back().sampler.mag_filter=TextureFilter::linear; }
    auto texture_gpu=take(r.prepare_scene(q,textured.scene())); auto texture_view=temporal_view(textured,3);
    schedule.target_spp=1; check(r.configure_restir_view(q,91,texture_gpu,texture_view,s,ris,schedule));
    auto initial=ris; initial.temporal.reset();
    compare_images(finish_view(r,q,91),complete(q,take(r.submit_restir_view(q,texture_gpu,texture_view,s,initial,{0,1}))),0);
    schedule.target_spp=7; schedule.batch_spp=1; check(r.configure_restir_view(q,91,texture_gpu,texture_view,s,ris,schedule));
    auto textured_history=finish_view(r,q,91); check(r.release_view(91));
    schedule.batch_spp=7; check(r.configure_restir_view(q,91,texture_gpu,texture_view,s,ris,schedule));
    compare_images(finish_view(r,q,91),textured_history,0); check(r.release_view(91));
    std::puts("temporal perspective/orthographic phases batch tile reverse and finite history=pass");
}
void temporal_state(memory::ResourceHandle heap,PathTraceRenderer& r,SubmissionQueue& q) {
    Fixture f{heap}; f.quad(-8,8,0,{.6f,.4f,.2f}); auto gpu=take(r.prepare_scene(q,f.scene()));
    auto view=temporal_view(f,3); auto s=settings(); s.environment_rgb=Vec3d::Ones(); auto ris=temporal_settings();
    PathTraceSchedule schedule; schedule.target_spp=4; schedule.batch_spp=2;
    check(r.configure_restir_view(q,92,gpu,view,s,ris,schedule)); auto original=finish_view(r,q,92); auto held=r.completed_frame(92);
    require(!r.submit_restir_view(q,gpu,view,s,ris) && !r.submit_restir_rays(q,gpu,std::span{&down,1},s,ris),"history-free temporal accepted");
    const auto generation=take(r.status(92)).generation;
    for(unsigned kind=0;kind<7;++kind) {
        auto invalid=ris;
        if(kind==0) invalid.temporal->max_history_age=0; if(kind==1) invalid.temporal->max_history_candidates=4097;
        if(kind==2) invalid.temporal->position_tolerance=NAN; if(kind==3) invalid.temporal->normal_cosine=1.1;
        if(kind==4) invalid.temporal->version=2; if(kind==5) invalid.max_candidate_evaluations=9;
        if(kind==6) invalid.temporal->position_tolerance=std::numeric_limits<double>::denorm_min();
        require(!r.configure_restir_view(q,92,gpu,view,s,invalid,schedule),"invalid temporal config accepted");
        require(take(r.status(92)).generation==generation && r.completed_frame(92).linear_buffer()==held.linear_buffer(),"reject changed history");
    }
    schedule.target_spp=6; schedule.exposure=2; check(r.configure_restir_view(q,92,gpu,view,s,ris,schedule));
    require(take(r.status(92)).generation==generation,"exposure reset temporal history");
    // Fail temporal and shade submission separately. The whole private batch must be discarded.
    for(unsigned phase:{3u,1u}) {
        do { require(take(r.submit_batch(q,92)),"phase submission missing"); require(!collect(r,q),"private phase published"); }
        while(take(r.status(92)).sampling_pass!=phase);
        fail_submit=true; require(!r.submit_batch(q,92),"phase failure not injected");
        require(take(r.status(92)).completed_spp==4 && r.completed_frame(92).linear_buffer()==held.linear_buffer(),"failed phase replaced history");
    }
    for(unsigned phase:{0u,3u,1u}) {
        if(phase) do { require(take(r.submit_batch(q,92)),"cancel phase missing"); require(!collect(r,q),"cancel phase published"); }
        while(take(r.status(92)).sampling_pass!=phase);
        require(take(r.submit_batch(q,92)),"cancel work missing"); check(r.cancel(92)); collect(r,q);
        require(take(r.status(92)).completed_spp==4 && r.completed_frame(92).linear_buffer()==held.linear_buffer(),"phase cancellation lost complete history");
    }
    for(unsigned tile=0;tile<3;++tile) { take(r.submit_batch(q,92)); require(!collect(r,q),"private epoch published"); }
    require(take(r.status(92)).sampling_epoch==5,"second private epoch missing");
    std::stop_source stop; take(r.submit_batch(q,92,stop.get_token())); stop.request_stop(); collect(r,q);
    require(take(r.status(92)).completed_spp==4,"late cancellation advanced sampling commit");
    auto replay=finish_view(r,q,92);
    check(r.configure_restir_view(q,93,gpu,view,s,ris,schedule)); compare_images(replay,finish_view(r,q,93),0); check(r.release_view(93));
    for(bool ortho:{false,true}) {
        check(r.release_view(92)); auto before=temporal_view(f,3,ortho);
        check(r.configure_restir_view(q,92,gpu,before,s,ris,schedule)); finish_view(r,q,92);
        schedule.target_spp=2; auto moved=temporal_view(f,3,ortho,.23,.025);
        check(r.configure_restir_view(q,92,gpu,moved,s,ris,schedule));
        require(take(r.status(92)).completed_spp==0 && !r.completed_frame(92),"camera retained old radiance");
        auto image=finish_view(r,q,92);
        require(image.info().sampling_first_epoch==6 && image.info().temporal_history_epoch==5,"camera lost sampling epoch");
        require(image.info().temporal_statistics.reused>0,"camera did not reproject history");
        // Stop moving: append radiance while keeping the sampling origin.
        schedule.target_spp=4; check(r.configure_restir_view(q,92,gpu,moved,s,ris,schedule));
        require(finish_view(r,q,92).info().sampling_first_epoch==6,"stopped camera reset epochs");
        schedule.target_spp=6;
    }
    compare_images(complete(q,take(held.read_linear(q))),original,0);
    // Late candidate from a moved camera cannot replace a reset generation.
    schedule.target_spp=8; auto current=temporal_view(f,3,true,.23,.025);
    check(r.configure_restir_view(q,92,gpu,current,s,ris,schedule)); take(r.submit_batch(q,92)); check(r.reset(92)); collect(r,q);
    require(!r.completed_frame(92) && take(r.status(92)).completed_spp==0,"late epoch committed after reset");
    schedule.target_spp=2; check(r.configure_restir_view(q,92,gpu,current,s,ris,schedule));
    require(finish_view(r,q,92).info().sampling_first_epoch==0,"reset kept random origin"); check(r.release_view(92));
    std::puts("temporal invalid inputs phase failures cancel camera epochs reset immutable owners=pass");
}
void temporal_boundaries(memory::ResourceHandle heap,PathTraceRenderer& r,SubmissionQueue& q) {
    Fixture f{heap}; f.quad(-8,8,0,{.6f,.4f,.2f}); auto gpu=take(r.prepare_scene(q,f.scene()));
    auto s=settings(); s.environment_rgb=Vec3d::Ones(); auto ris=temporal_settings(); auto base=temporal_view(f,4);
    PathTraceSchedule schedule; schedule.target_spp=3; schedule.batch_spp=2;
    for(unsigned change=0;change<9;++change) {
        check(r.configure_restir_view(q,96,gpu,base,s,ris,schedule)); finish_view(r,q,96);
        auto moved=temporal_view(f,4,false,.22); check(r.configure_restir_view(q,96,gpu,moved,s,ris,schedule));
        require(finish_view(r,q,96).info().sampling_first_epoch==3,"camera history setup failed");
        auto p=s; auto config=ris; auto scheduling=schedule; auto d=moved.description(); auto scene=gpu;
        if(change==0) d.projection(0,0)*=1.1;
        if(change==1) d.width=5;
        if(change==2) ++p.seed;
        if(change==3) p.environment_rgb*=.5;
        if(change==4) ++config.initial_candidates;
        if(change==5) ++config.temporal->max_history_age;
        if(change==6) ++scheduling.gesture;
        if(change==7) scene=take(r.prepare_scene(q,f.scene()));
        if(change==8) d.projection=take(orthographic_projection({1,.1,10},1));
        auto changed=take(RenderView::create(f.source(),d)); check(r.configure_restir_view(q,96,scene,changed,p,config,scheduling));
        require(!r.completed_frame(96),"reset retained incompatible radiance"); auto actual=finish_view(r,q,96);
        require(actual.info().sampling_first_epoch==0 && !actual.info().temporal_history_epoch,"physical/lens/reset retained incompatible sampling history");
        check(r.configure_restir_view(q,97,scene,changed,p,config,scheduling)); compare_images(actual,finish_view(r,q,97),0);
        check(r.release_view(96)); check(r.release_view(97));
    }
    // Two independently owned chains and a camera jump/disocclusion must not share reservoirs.
    check(r.configure_restir_view(q,96,gpu,base,s,ris,schedule)); auto original=finish_view(r,q,96); auto held=r.completed_frame(96);
    auto far=temporal_view(f,4,false,20); check(r.configure_restir_view(q,96,gpu,far,s,ris,schedule));
    auto revealed=finish_view(r,q,96); require(revealed.info().temporal_statistics.reused==0,"out-of-view history reused");
    for(const auto& color:revealed.linear_rgb()) close_color(color,s.environment_rgb,0);
    check(r.configure_restir_view(q,97,gpu,base,s,ris,schedule)); compare_images(finish_view(r,q,97),original,0);
    require(!r.configure_restir_view(q,98,gpu,base,s,ris,schedule),"third temporal view accepted");
    compare_images(complete(q,take(held.read_linear(q))),original,0); check(r.release_view(96)); check(r.release_view(97));
    // Parallax reveals receiver points whose previous projected pixel saw the foreground.
    Fixture occluded{heap}; occluded.quad(-4,4,0,{.6f,.4f,.2f}); occluded.quad(-.3f,.3f,.5f,{0,0,0});
    auto blocked=take(r.prepare_scene(q,occluded.scene())); auto front=temporal_view(occluded,5,false,0);
    schedule.target_spp=4; check(r.configure_restir_view(q,96,blocked,front,s,ris,schedule)); finish_view(r,q,96);
    auto peek=temporal_view(occluded,5,false,.4); schedule.target_spp=1;
    check(r.configure_restir_view(q,96,blocked,peek,s,ris,schedule)); auto disoccluded=finish_view(r,q,96);
    auto fresh=complete(q,take(r.submit_restir_view(q,blocked,peek,s,{}, {4,1})));
    require(disoccluded.primary_hits()[12] && disoccluded.primary_hits()[12]->index<2 &&
        disoccluded.info().temporal_statistics.rejected>0,"parallax disocclusion fixture inactive");
    close_color(disoccluded.linear_rgb()[12],fresh.linear_rgb()[12],0); check(r.release_view(96));
    // Small footprints may reject history because projection magnifies float position error.
    // Keep this a fallback/accounting test: another GPU may match every sample successfully.
    auto tiny=temporal_view(f,1,false,.3,0,1e-5); auto tiny_settings=s; tiny_settings.seed=101;
    schedule.target_spp=256; schedule.batch_spp=16;
    check(r.configure_restir_view(q,96,gpu,tiny,tiny_settings,ris,schedule));
    const auto tiny_image=finish_view(r,q,96); const auto& tiny_stats=tiny_image.info().temporal_statistics;
    require(tiny_stats.reused+tiny_stats.rejected+tiny_stats.expired==256,"tiny footprint lost temporal decisions");
    std::printf("tiny footprint reused=%llu rejected=%llu expired=%llu\n",static_cast<unsigned long long>(tiny_stats.reused),
        static_cast<unsigned long long>(tiny_stats.rejected),static_cast<unsigned long long>(tiny_stats.expired));
    check(r.release_view(96)); schedule.batch_spp=2;
    // Retained old frames plus both temporal ping-pong planes are charged before publication.
    auto bounded=take(PathTraceRenderer::create(heap,q,DK_PATH_SHADER_DIR,{},1ull<<20)); auto small=take(bounded.prepare_scene(q,f.scene()));
    schedule.target_spp=2; auto view=temporal_view(f,32); auto too_large=temporal_view(f,64);
    require(!bounded.configure_restir_view(q,1,small,too_large,s,ris,schedule),"temporal planes bypassed budget");
    check(bounded.configure_restir_view(q,1,small,view,s,ris,schedule)); advance_view(bounded,q,1); auto a=bounded.completed_frame(1);
    check(bounded.reset(1)); advance_view(bounded,q,1); auto b=bounded.completed_frame(1);
    check(bounded.reset(1)); advance_view(bounded,q,1); auto before=bounded.completed_frame(1).linear_buffer();
    schedule.target_spp=4; check(bounded.configure_restir_view(q,1,small,view,s,ris,schedule));
    require(!bounded.submit_batch(q,1) && bounded.completed_frame(1).linear_buffer()==before,"budget failure replaced completed sampling owner");
    a={}; b={}; advance_view(bounded,q,1); check(bounded.release_view(1));
    std::puts("temporal reset matrix disocclusion independent views immutable history and memory recovery=pass");
}
void temporal_support(memory::ResourceHandle heap,PathTraceRenderer& r,SubmissionQueue& q) {
    Fixture f{heap}; f.quad(-4,4,0,{.6f,.4f,.2f});
    // One-sided vertical area light has disjoint support across x=0 on one receiver primitive.
    f.triangle({0,-1,.4f},{0,1,.4f},{0,0,2},{0,0,0},{4,3,2});
    // A thin, opaque alpha-mask obstacle tests current visibility; primary orthographic rays miss it.
    f.triangle({.15f,-.4f,.2f},{.15f,.4f,.2f},{.15f,0,.8f},{0,0,0},{0,0,0},true);
    f.asset.materials.back().alpha_mode=AlphaMode::mask;
    auto gpu=take(r.prepare_scene(q,f.scene())); auto d=temporal_view(f,1,true,0,0,1).description();
    Trsd camera; camera.translation={0,-.6,1}; d.camera_world=take(Transformd::from_trs(camera)); auto view=take(RenderView::create(f.source(),d));
    auto s=settings(); s.environment_rgb={.2,.3,.4}; auto ris=temporal_settings(); ris.temporal->position_tolerance=2;
    ris.temporal->max_history_candidates=4; ris.temporal->max_history_age=4;
    std::array<Vec3d,8> observations,reference; std::uint64_t corrections=0,reused=0;
    for(unsigned chain=0;chain<8;++chain) {
        s.seed=101+7919*chain; PathTraceSchedule schedule; schedule.target_spp=4096; schedule.batch_spp=16;
        check(r.configure_restir_view(q,98,gpu,view,s,ris,schedule)); auto image=finish_view(r,q,98);
        observations[chain]=image.linear_rgb()[0]; corrections+=image.info().temporal_statistics.support_corrected;
        reused+=image.info().temporal_statistics.reused;
        reference[chain]=take(render_path_reference(heap,gpu.source(),view,s,{0,65536})).linear_rgb()[0];
        check(r.release_view(98));
    }
    temporal_mean(observations,reference); require(corrections>0 && reused>1000,"GPU support correction fixture inactive");
    std::printf("temporal support corrections=%llu reused=%llu mask current visibility=pass\n",
        static_cast<unsigned long long>(corrections),static_cast<unsigned long long>(reused));
}
}

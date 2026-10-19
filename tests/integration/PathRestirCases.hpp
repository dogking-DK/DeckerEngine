#pragma once
namespace {
PathTraceGpuImage ris_ray(PathTraceRenderer& r,SubmissionQueue& q,const PathTraceGpuScene& scene,const PathTraceSettings& s,
    std::uint32_t count,const RestirDiSettings& ris={},const PathTraceRay& ray=down) {
    return complete(q,take(r.submit_restir_rays(q,scene,std::span{&ray,1},s,ris,{0,count})));
}
void restir_population(PathTraceRenderer& r,SubmissionQueue& q,const PathTraceGpuScene& gpu,PathTraceSettings s,
    std::uint32_t count=16384,const RestirDiSettings& ris={},const PathTraceRay& ray=down,const Vec3d* analytic=nullptr) {
    std::array<Vec3d,8> values,references; Vec3d mean=Vec3d::Zero(),ref=Vec3d::Zero(); double difference=0,norm=0;
    for(unsigned i=0;i<8;++i) {
        s.seed=101+7919*i; values[i].setZero();
        const auto chunk=PathTraceRenderer::restir_max_candidates_per_invocation/ris.initial_candidates;
        for(std::uint32_t first=0;first<count;first+=chunk) {
            const auto size=std::min(chunk,count-first);
            const auto image=complete(q,take(r.submit_restir_rays(q,gpu,std::span{&ray,1},s,ris,{first,size})));
            require(image.info().restir && image.info().restir->initial_candidates==ris.initial_candidates,"RIS source settings missing");
            values[i]+=image.linear_rgb()[0]*(double(size)/count);
        }
        references[i]=take(estimate_restir_reference(gpu.source(),ray,s,ris,{0,count})).mean;
        mean+=values[i]/8; ref+=references[i]/8; difference+=(values[i]-references[i]).squaredNorm(); norm+=references[i].squaredNorm();
    }
    for(unsigned c=0;c<3;++c) {
        double variance=0;
        for(unsigned i=0;i<8;++i) variance+=(values[i][c]-mean[c])*(values[i][c]-mean[c])+(references[i][c]-ref[c])*(references[i][c]-ref[c]);
        const double error=std::abs(mean[c]-ref[c]),se=std::sqrt(variance/56);
        if(error>.02*std::abs(ref[c])+1e-4 || error>5*se+1e-4)
            std::printf("RIS mismatch channel=%u GPU=%.12g CPU=%.12g SE=%.12g ray=(%.8g,%.8g,%.8g)\n",c,mean[c],ref[c],se,ray.direction.x(),ray.direction.y(),ray.direction.z());
        require(error<=.02*std::abs(ref[c])+1e-4 && error<=5*se+1e-4,"RIS CPU/GPU population differs");
        if(analytic) require(std::abs(mean[c]-(*analytic)[c])<=.02*std::abs((*analytic)[c])+1e-4 &&
            std::abs(mean[c]-(*analytic)[c])<=5*se+1e-4,"RIS independent analytic energy differs");
    }
    const double nrmse=std::sqrt(difference/std::max(1e-12,norm));
    std::printf("RIS samples=%u candidates=%u mean=(%.8g,%.8g,%.8g) CPU nRMSE=%.8g\n",count,ris.initial_candidates,mean.x(),mean.y(),mean.z(),nrmse);
    require(nrmse<=.02,"RIS same-counter CPU/GPU nRMSE exceeds 2%");
}
void restir_numeric(memory::ResourceHandle heap,PathTraceRenderer& r,SubmissionQueue& q) {
    Fixture empty{heap}; auto s=settings(); s.environment_rgb={.25,.5,.75};
    const auto sky=take(r.prepare_scene(q,empty.scene())); close_color(ris_ray(r,q,sky,s,16).linear_rgb()[0],s.environment_rgb,0);
    Fixture f{heap}; f.quad(-8,8,0,{.6f,.4f,.2f}); auto cpu=f.scene(); auto gpu=take(r.prepare_scene(q,cpu));
    s.environment_rgb=Vec3d::Ones(); const Vec3d expected=Vec3f{.6f,.4f,.2f}.cast<double>();
    restir_population(r,q,gpu,s,16384,{},down,&expected);
    RestirDiSettings one; one.initial_candidates=1; restir_population(r,q,gpu,s,65536,one,down,&expected);
    s.environment_rgb.setZero(); close_color(ris_ray(r,q,gpu,s,4).linear_rgb()[0],Vec3d::Zero(),0);
    EnvironmentDescription env; env.sun.direction={0,0,1}; env.sun.irradiance={2,3,4}; f.document->set_environment(env).value();
    gpu=take(r.prepare_scene(q,f.scene())); s.sun=PathTraceSun::scene;
    close_color(ris_ray(r,q,gpu,s,8).linear_rgb()[0],expected.cwiseProduct(Vec3d{2,3,4})/std::numbers::pi,1e-6);
    f.quad(-1,1,2,{0,0,0},{4,3,2},true); gpu=take(r.prepare_scene(q,f.scene()));
    s.environment_rgb={.4,.3,.2}; restir_population(r,q,gpu,s);
    s.sun=PathTraceSun::none; s.environment_rgb.setZero(); restir_population(r,q,gpu,s);
    const auto before=ris_ray(r,q,gpu,s,64);
    const PathTraceRay up{{0,0,1},{0,0,1}};
    close_color(ris_ray(r,q,gpu,s,4,{},up).linear_rgb()[0],{4,3,2},0);
    auto zero=settings(0); require(!r.submit_restir_rays(q,gpu,std::span{&down,1},zero,{}, {0,8}),"zero bounce request accepted");
    RestirDiSettings excessive; excessive.initial_candidates=64;
    require(!r.submit_restir_rays(q,gpu,std::span{&down,1},s,excessive,{0,65536}),"unbounded per-invocation RIS work accepted");
    auto negative_zero=down; negative_zero.minimum=-0.0; negative_zero.direction.x()=-0.0;
    compare_images(ris_ray(r,q,gpu,s,16,{},negative_zero),ris_ray(r,q,gpu,s,16),0);
    compare_images(ray_image(r,q,gpu,s,16,negative_zero),ray_image(r,q,gpu,s,16),0);
    auto medium=take(PathMediumSnapshot::create(f.source().environment()));
    VolumePathSettings volume; volume.max_bounces=1; volume.rr_start=0; volume.sun=false;
    compare_images(complete(q,take(r.submit_medium_rays(q,gpu,medium,std::span{&negative_zero,1},volume,{0,16}))),
        complete(q,take(r.submit_medium_rays(q,gpu,medium,std::span{&down,1},volume,{0,16}))),0);
    const std::array rays{down,negative_zero};
    const auto batch=complete(q,take(r.submit_restir_rays(q,gpu,rays,s,{}, {11,1024},17)));
    for(unsigned i=0;i<2;++i) close_color(batch.linear_rgb()[i],take(estimate_restir_reference(gpu.source(),rays[i],s,{}, {11,1024},17+i)).mean,1e-5);
    require(before.info().restir->version==1,"RIS version missing");
}
void restir_materials(memory::ResourceHandle heap,PathTraceRenderer& r,SubmissionQueue& q) {
    Fixture f{heap}; f.quad(-8,8,0,{.7f,.4f,.2f}); f.quad(-1,1,2,{.2f,.3f,.4f},{3,2,1},true);
    auto s=settings(3); s.rr_start=1; s.environment_rgb={.3,.4,.5};
    restir_population(r,q,take(r.prepare_scene(q,f.scene())),s);
    for(std::size_t i=2;i<f.asset.materials.size();++i) f.asset.materials[i].alpha_mode=AlphaMode::mask;
    auto masked=take(r.prepare_scene(q,f.scene())); require(masked.source().masked_emitter_count()==2,"mask fixture absent");
    restir_population(r,q,masked,s,32768);
    for(std::size_t i=2;i<f.asset.materials.size();++i) f.asset.materials[i].base_color.w()=0;
    restir_population(r,q,take(r.prepare_scene(q,f.scene())),s);
    for(auto& m:f.asset.materials) { m.alpha_mode=AlphaMode::opaque; m.base_color.w()=1; }
    for(float rough:{.4f,.05f}) {
        f.asset.materials[0].roughness=rough; f.asset.materials[1].roughness=rough;
        f.asset.materials[0].metallic=.7f; f.asset.materials[1].metallic=.7f;
        auto gpu=take(r.prepare_scene(q,ggx_scene(f))); RestirDiSettings ris; ris.initial_candidates=rough<.1f ? 64 : 8;
        auto surface=settings(); restir_population(r,q,gpu,surface,rough<.1f ? 65536 : 16384,ris);
        // Stay off the diagonal shared edge: double Moller and hardware traversal have different tie rules.
        if(rough>.1f) { const PathTraceRay grazing{{1.19,.13,.1},Vec3d{-.99,0,-.1}.normalized()}; restir_population(r,q,gpu,surface,32768,ris,grazing); }
    }
    // Opaque blocker changes current visibility without changing the receiver/light proposal.
    f.quad(-4,4,1.5,{0,0,0},{0,0,0},true); s=settings();
    close_color(ris_ray(r,q,take(r.prepare_scene(q,ggx_scene(f))),s,64).linear_rgb()[0],Vec3d::Zero(),0);
}
void restir_textures(memory::ResourceHandle heap,PathTraceRenderer& r,SubmissionQueue& q) {
    Fixture f{heap}; f.quad(-4,4,0,{.8f,.7f,.6f});
    for(unsigned i:{0u,1u}) { add_texture(f,i,{.37f,.63f}); f.asset.textures.back().sampler.mag_filter=TextureFilter::linear; }
    auto s=settings(); s.environment_rgb=Vec3d::Ones(); auto gpu=take(r.prepare_scene(q,f.scene()));
    // Separate hardware filtering precision from Monte Carlo error. The existing texture
    // contract allows .005 absolute; RIS must match the same GPU-filtered furnace mean.
    auto classic=s; classic.estimator=PathTraceEstimator::bsdf_only;
    const Vec3d filtered=ray_image(r,q,gpu,classic,16).linear_rgb()[0];
    const Vec3d filtered_cpu=take(estimate_path_reference(gpu.source(),down,classic,{0,16})).mean;
    close_color(filtered,filtered_cpu,.005);
    std::printf("hardware texture absolute difference=%.9g\n",(filtered-filtered_cpu).cwiseAbs().maxCoeff());
    std::array<Vec3d,8> observations; Vec3d average=Vec3d::Zero();
    for(unsigned i=0;i<8;++i) { s.seed=101+7919*i; observations[i]=ris_ray(r,q,gpu,s,16384).linear_rgb()[0]; average+=observations[i]/8; }
    for(unsigned c=0;c<3;++c) {
        double variance=0; for(const auto& v:observations) variance+=(v[c]-average[c])*(v[c]-average[c]);
        const auto error=std::abs(average[c]-filtered[c]);
        require(error<=.02*std::abs(filtered[c])+1e-4 && error<=5*std::sqrt(variance/56)+1e-4,"RIS changed GPU-filtered furnace energy");
    }
    // Binary interpolation weights permit the strict CPU/GPU statistical oracle without
    // a deterministic hardware filter floor masquerading as sampling bias.
    for(unsigned i:{0u,1u}) f.asset.mesh.primitives[i].texcoords={{.375f,.625f},{.375f,.625f},{.375f,.625f}};
    f.quad(-1,1,2,{0,0,0},{4,3,2},true); f.quad(-.4f,.4f,1.5f,{0,0,0});
    for(unsigned i:{4u,5u}) { add_texture(f,i); f.asset.materials[i].alpha_mode=AlphaMode::mask; f.asset.materials[i].double_sided=true; }
    s.environment_rgb={.2,.3,.4};
    gpu=take(r.prepare_scene(q,f.scene())); require(gpu.source().texture_count()==4,"RIS texture fixture incomplete");
    restir_population(r,q,gpu,s);
    auto view=f.view(f.source(),5); PathTraceSchedule schedule; schedule.target_spp=7; schedule.batch_spp=3; schedule.tile_edge=3;
    check(r.configure_restir_view(q,76,gpu,view,s,{},schedule));
    compare_images(finish_view(r,q,76),complete(q,take(r.submit_restir_view(q,gpu,view,s,{}, {0,7}))),0); check(r.release_view(76));
    for(unsigned i:{4u,5u}) f.asset.mesh.primitives[i].texcoords={{.75f,.25f},{.75f,.25f},{.75f,.25f}};
    for(unsigned i:{2u,3u}) f.asset.materials[i].double_sided=true;
    gpu=take(r.prepare_scene(q,f.scene())); restir_population(r,q,gpu,s);
    const PathTraceRay back{{.2,.1,3},{0,0,-1}}; close_color(ris_ray(r,q,gpu,s,1,{},back).linear_rgb()[0],{4,3,2},0);
    std::puts("RIS texture filtering/masked shadows/double sided/phased textured bindings=pass");
}
void restir_schedule(memory::ResourceHandle heap,PathTraceRenderer& r,SubmissionQueue& q) {
    Fixture f{heap}; f.quad(-8,8,0,{.6f,.4f,.2f}); f.quad(-1,1,2,{.2f,.3f,.4f},{4,3,2},true);
    const auto gpu=take(r.prepare_scene(q,ggx_scene(f))); auto s=settings(3); s.environment_rgb={.3,.4,.5};
    for(bool ortho:{false,true}) {
        auto d=f.view(f.source(),5).description(); if(ortho) d.projection=take(orthographic_projection({2,.1,10},1));
        auto view=take(RenderView::create(f.source(),d));
        auto base=complete(q,take(r.submit_restir_view(q,gpu,view,s,{}, {0,19})));
        for(unsigned batch:{1u,7u,16u}) {
            std::printf("RIS schedule ortho=%u batch=%u\n",unsigned(ortho),batch);
            PathTraceSchedule schedule; schedule.target_spp=19; schedule.batch_spp=batch; schedule.tile_edge=batch==1 ? 64 : 3; schedule.reverse_tiles=batch==16;
            check(r.configure_restir_view(q,71,gpu,view,s,{},schedule));
            compare_images(finish_view(r,q,71),base,0); check(r.release_view(71));
        }
        // Camera-derived fixed rays (including -0 TMin) are checked independently of phased rendering.
        for(unsigned y=0;y<5;++y) for(unsigned x=0;x<5;++x) {
            const auto ray=take(path_trace_camera_ray(view,x,y,{.5,.5}));
            auto actual=ris_ray(r,q,gpu,s,1024,{},ray); auto oracle=take(estimate_restir_reference(gpu.source(),ray,s,{}, {0,1024}));
            close_color(actual.linear_rgb()[0],oracle.mean,.02*oracle.mean.norm()+1e-4);
        }
    }
    Fixture sky{heap}; auto empty=take(r.prepare_scene(q,sky.scene())); auto view=sky.view(sky.source(),3);
    PathTraceSchedule schedule; schedule.target_spp=3; schedule.batch_spp=2; schedule.tile_edge=2;
    check(r.configure_restir_view(q,71,empty,view,s,{},schedule));
    compare_images(finish_view(r,q,71),complete(q,take(r.submit_restir_view(q,empty,view,s,{}, {0,3}))),0);
    check(r.release_view(71)); std::puts("RIS phases/epochs/perspective/orthographic/batches/reverse tiles=pass");
}
void restir_state(memory::ResourceHandle heap,PathTraceRenderer& r,SubmissionQueue& q) {
    Fixture f{heap}; f.quad(-8,8,0,{.6f,.4f,.2f}); auto gpu=take(r.prepare_scene(q,f.scene())); auto view=f.view(f.source(),4);
    auto s=settings(); s.environment_rgb=Vec3d::Ones(); PathTraceSchedule schedule; schedule.target_spp=2; schedule.batch_spp=2; schedule.tile_edge=2;
    check(r.configure_restir_view(q,81,gpu,view,s,{},schedule)); auto original=finish_view(r,q,81); auto held=r.completed_frame(81);
    const auto generation=take(r.status(81)).generation;
    schedule.target_spp=4; schedule.exposure=2; schedule.reverse_tiles=true;
    check(r.configure_restir_view(q,81,gpu,view,s,{},schedule)); require(take(r.status(81)).generation==generation,"display/schedule reset RIS");
    RestirDiSettings invalid; invalid.version=2; require(!r.configure_restir_view(q,81,gpu,view,s,invalid,schedule),"unknown version accepted");
    auto wrong=s; wrong.estimator=PathTraceEstimator::bsdf_only; require(!r.configure_restir_view(q,81,gpu,view,wrong,{},schedule),"RIS BSDF-only accepted");
    invalid={}; invalid.max_candidate_evaluations=7; require(!r.configure_restir_view(q,81,gpu,view,s,invalid,schedule),"one-pixel candidate budget accepted");
    require(r.completed_frame(81).linear_buffer()==held.linear_buffer(),"reject lost completed owner");
    // Finish every initial tile; no new sample may be published before shade completes.
    for(unsigned i=0;i<4;++i) { require(take(r.submit_batch(q,81)),"initial tile missing"); require(!collect(r,q),"initial phase published"); }
    auto progress=take(r.status(81)); require(progress.completed_spp==2 && progress.sampling_pass==1 && progress.sampling_epoch==2,"phase boundary count incorrect");
    fail_submit=true; const auto failed=r.submit_batch(q,81);
    require(!failed && r.completed_frame(81).linear_buffer()==held.linear_buffer(),"phase submit failure published candidate");
    compare_images(complete(q,take(held.read_linear(q))),original,0);
    // Cancel an in-flight initial tile, then replay from the last committed sample.
    require(take(r.submit_batch(q,81)),"retry tile missing"); check(r.cancel(81)); collect(r,q);
    require(take(r.status(81)).completed_spp==2,"cancel advanced spp");
    compare_images(finish_view(r,q,81),complete(q,take(r.submit_restir_view(q,gpu,view,s,{}, {0,4}))),0);
    schedule.target_spp=6; check(r.configure_restir_view(q,81,gpu,view,s,{},schedule));
    // One whole private epoch must remain unpublished when a later epoch is cancelled.
    for(unsigned i=0;i<8;++i) { require(take(r.submit_batch(q,81)),"private epoch tile missing"); require(!collect(r,q),"private epoch published early"); }
    require(take(r.status(81)).sampling_epoch==5 && take(r.status(81)).completed_spp==4,"private epoch count leaked");
    std::stop_source stop; require(take(r.submit_batch(q,81,stop.get_token())),"stoppable epoch missing"); stop.request_stop(); collect(r,q);
    require(take(r.status(81)).completed_spp==4,"late epoch cancellation advanced spp");
    compare_images(finish_view(r,q,81),complete(q,take(r.submit_restir_view(q,gpu,view,s,{}, {0,6}))),0);
    schedule.target_spp=8; check(r.configure_restir_view(q,81,gpu,view,s,{},schedule)); require(take(r.submit_batch(q,81)),"late tile missing");
    auto changed=s; ++changed.seed; check(r.configure_restir_view(q,81,gpu,view,changed,{},schedule)); collect(r,q);
    require(take(r.status(81)).completed_spp==0 && !r.completed_frame(81),"late generation published");
    schedule.target_spp=1; check(r.configure_restir_view(q,82,gpu,view,s,{},schedule)); finish_view(r,q,82);
    require(!r.configure_restir_view(q,83,gpu,view,s,{},schedule),"third view accepted");
    check(r.release_view(81)); check(r.release_view(82));
    compare_images(complete(q,take(held.read_linear(q))),original,0);
    auto bounded=take(PathTraceRenderer::create(heap,q,DK_PATH_SHADER_DIR,{},1ull<<20)); auto small=take(bounded.prepare_scene(q,f.scene()));
    auto large=f.view(f.source(),64); require(!bounded.configure_restir_view(q,1,small,large,s,{},schedule),"RIS planes bypass memory limit");
    check(bounded.configure_restir_view(q,1,small,view,s,{},schedule)); finish_view(bounded,q,1); auto old=bounded.completed_frame(1);
    const auto owned=bounded.memory_stats(q).owned.current; check(bounded.release_view(1));
    require(bounded.memory_stats(q).owned.current==owned,"external completed owner not charged"); old={};
    require(bounded.memory_stats(q).owned.current+224*16<=owned,"RIS plane owner did not release bytes");
    // Candidate rejection caused by external full-frame owners must be recoverable.
    schedule.target_spp=2; schedule.batch_spp=2; schedule.tile_edge=64;
    auto medium_view=f.view(f.source(),32); check(bounded.configure_restir_view(q,1,small,medium_view,s,{},schedule)); advance_view(bounded,q,1);
    auto first=bounded.completed_frame(1); check(bounded.reset(1)); advance_view(bounded,q,1);
    auto second=bounded.completed_frame(1); check(bounded.reset(1)); advance_view(bounded,q,1);
    const auto retained=bounded.completed_frame(1).linear_buffer(); schedule.target_spp=4;
    check(bounded.configure_restir_view(q,1,small,medium_view,s,{},schedule));
    require(!bounded.submit_batch(q,1) && bounded.completed_frame(1).linear_buffer()==retained,"held owners bypassed candidate budget");
    first={}; second={}; advance_view(bounded,q,1); check(bounded.release_view(1));
    std::puts("RIS invalid/budget/two views/phase failure/cancel/late/immutable owner=pass");
}
}

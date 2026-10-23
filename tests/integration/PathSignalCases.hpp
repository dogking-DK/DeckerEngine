#pragma once
namespace {
Vec3d signal_rgb(const DenoisingSignalPixel::Float4& v) { return {v[0],v[1],v[2]}; }
void signal_recompose(const PathTraceGpuImage& image) {
    require(image.info().signals.has_value(),"signal provenance missing");
    const auto& source=image.info().signals->source;
    require(source.version==1 && source.source_serial && source.renderer_id && source.width==image.info().width &&
        source.height==image.info().height && source.fingerprint==image.info().fingerprint &&
        source.sample_first+source.sample_count==source.accumulated_spp,"signal source identity mismatch");
    require(image.denoising_signals().size()==image.linear_rgb().size(),"partial signal image published");
    for(const auto& p:image.denoising_signals()) {
        const Vec3d raw=signal_rgb(p.fresh_sum),sum=signal_rgb(p.diffuse_sum)+signal_rgb(p.specular_sum)+signal_rgb(p.emission_sum)+signal_rgb(p.background_sum);
        close_color(sum,raw,2e-5*std::max(1.,raw.maxCoeff()));
        require(p.guide_samples()==source.accumulated_spp && p.identity[3]==source.sample_count && p.identity[1]==source.sampling_first_epoch,"guide/fresh coverage mismatch");
    }
}
RenderView signal_view(const Fixture& f,std::uint32_t width=7,std::uint32_t height=5,bool ortho=false,double x=0) {
    auto d=f.view(f.source(),width).description(); d.height=height;
    if(ortho) d.projection=take(orthographic_projection({2,.25,4},double(width)/height));
    Trsd pose; pose.translation={x,0,1}; d.camera_world=take(Transformd::from_trs(pose));
    return take(RenderView::create(f.source(),d));
}
void signal_numeric(memory::ResourceHandle heap,PathTraceRenderer& r,SubmissionQueue& q) {
    for(unsigned mode=0;mode<6;++mode) {
        Fixture f{heap}; f.quad(-10,10,0,{.7f,.4f,.2f},{.03f,.02f,.01f}); f.quad(-1,1,2,{.3f,.3f,.3f},{2,3,4},true);
        ggx_material(f,mode==0 ? 0.f : (mode==2 ? 1.f : .4f),mode==2 ? .05f : .4f);
        EnvironmentDescription env; env.sun.direction={0,0,1}; env.sun.irradiance={.5,.7,1}; check(f.document->set_environment(env));
        const auto cpu=mode==0 ? f.scene() : ggx_scene(f); const auto scene=take(r.prepare_scene(q,cpu)); const auto view=signal_view(f);
        auto s=settings(3); s.rr_start=1; s.surface_only=true; s.environment_rgb={.2,.3,.4}; s.sun=PathTraceSun::scene;
        if(mode==0) s.estimator=PathTraceEstimator::bsdf_only;
        RestirDiSettings ris; if(mode>=4) ris.temporal.emplace(); if(mode>=5) ris.spatial.emplace();
        PathTraceSchedule schedule; schedule.target_spp=8; schedule.batch_spp=4; schedule.tile_edge=2; schedule.tiles_per_submission=3;
        auto configure=[&](std::uint64_t id) { if(mode>=3) check(r.configure_restir_view(q,id,scene,view,s,ris,schedule)); else check(r.configure_view(q,id,scene,view,s,schedule)); };
        configure(1); const auto off=finish_view(r,q,1); require(!off.info().signals && off.denoising_signals().empty() && !r.completed_frame(1).signal_buffer(),"Off allocated signals");
        schedule.signals.enabled=true; schedule.reverse_tiles=true; configure(2); const auto on=finish_view(r,q,2);
        compare_images(off,on,0); require(off.info().statistics.rays==on.info().statistics.rays,"signals added rays"); signal_recompose(on);
        PathTraceGpuImage independent;
        if(mode<3) independent=complete(q,take(r.submit_view(q,scene,view,s,{4,4})));
        else if(mode==3) independent=complete(q,take(r.submit_restir_view(q,scene,view,s,ris,{4,4})));
        for(std::size_t i=0;i<on.linear_rgb().size();++i) {
            const auto& p=on.denoising_signals()[i];
            if(independent) { close_color(signal_rgb(p.fresh_sum)/4,independent.linear_rgb()[i],0); close_color(signal_rgb(p.fresh_second_sum)/4,independent.second_moment()[i],0); }
            close_color(signal_rgb(p.albedo_sum)/8,Vec3f{.7f,.4f,.2f}.cast<double>(),1e-6);
            close_color(signal_rgb(p.normal_sum)/8,{0,0,1},0);
            close_color(signal_rgb(p.emission_sum)/4,Vec3f{.03f,.02f,.01f}.cast<double>(),1e-7);
            if(mode==0) close_color(signal_rgb(p.specular_sum),Vec3d::Zero(),0);
            if(mode==2) close_color(signal_rgb(p.diffuse_sum),Vec3d::Zero(),0);
            require(p.guide_hits()==8 && p.position_view_depth[3]==1 && (p.identity[2]&1),"surface guide mismatch");
        }
        // A new schedule only affects the next batch. Keep a completed input alive during reuse.
        const auto old=r.completed_frame(2); const auto serial=old.info().signals->source.source_serial;
        schedule.batch_spp=1; schedule.target_spp=9; configure(2); const auto next=finish_view(r,q,2);
        signal_recompose(next); require(next.info().signals->source.sample_first==8 && next.info().signals->source.source_serial==serial+1,"fresh interval or serial advanced incorrectly");
        const auto retained=complete(q,take(old.read_linear(q)));
        require(std::memcmp(retained.denoising_signals().data(),on.denoising_signals().data(),on.denoising_signals().size_bytes())==0,"external signal owner overwritten");
        check(r.release_view(1)); check(r.release_view(2));
    }
    // Independent diffuse/specular analytic oracle for a mixed GGX BRDF and delta sun.
    for(float roughness:{0.f,.2f,1.f}) {
        Fixture f{heap}; f.quad(-10,10,0,{.7f,.4f,.2f}); ggx_material(f,.35f,roughness);
        EnvironmentDescription env; env.sun.direction={.6,0,.8}; env.sun.irradiance={2,3,4}; check(f.document->set_environment(env));
        const auto cpu=ggx_scene(f); const auto gpu=take(r.prepare_scene(q,cpu)); const auto view=signal_view(f,3,3);
        auto config=settings(1); config.sun=PathTraceSun::scene; config.surface_only=true;
        PathTraceSchedule schedule; schedule.signals.enabled=true; schedule.target_spp=schedule.batch_spp=3;
        for(bool ris:{false,true}) {
            if(ris) check(r.configure_restir_view(q,1,gpu,view,config,{},schedule)); else check(r.configure_view(q,1,gpu,view,config,schedule));
            const auto result=finish_view(r,q,1); signal_recompose(result);
            for(std::uint32_t i=0;i<9;++i) {
                Vec3d diffuse=Vec3d::Zero(),total=Vec3d::Zero(); const Vec3d color=Vec3f{.7f,.4f,.2f}.cast<double>();
                for(unsigned sample=0;sample<3;++sample) {
                    const auto ray=take(path_trace_camera_ray(view,i%3,i/3,{path_trace_sample(config.seed,i,sample,0,0),path_trace_sample(config.seed,i,sample,0,1)}));
                    const Vec3d wo=-ray.direction,wi{.6,0,.8};
                    const double transmission=.96*.96*(1-std::pow(1-wo.z(),5))*(1-std::pow(1-wi.z(),5));
                    diffuse+=color.cwiseProduct(Vec3d{2,3,4})*((1-double(.35f))*transmission*.8/std::numbers::pi);
                    total+=path_oracle::evaluate(color,.35f,roughness,wo,wi).cwiseProduct(Vec3d{2,3,4})*.8;
                }
                const auto& p=result.denoising_signals()[i]; close_color(signal_rgb(p.diffuse_sum),diffuse,3e-6);
                close_color(signal_rgb(p.specular_sum),total-diffuse,2e-3*std::max(1.,total.maxCoeff()));
            }
            check(r.release_view(1));
        }
    }
    Fixture empty{heap}; const auto scene=take(r.prepare_scene(q,empty.scene())); auto s=settings(); s.environment_rgb={.2,.4,.8};
    PathTraceSchedule schedule; schedule.signals.enabled=true; schedule.target_spp=5; schedule.batch_spp=3; schedule.tile_edge=2;
    for(bool ris:{false,true}) {
        const auto view=signal_view(empty);
        if(ris) check(r.configure_restir_view(q,1,scene,view,s,{},schedule)); else check(r.configure_view(q,1,scene,view,s,schedule));
        const auto image=finish_view(r,q,1); signal_recompose(image);
        for(const auto& p:image.denoising_signals()) { close_color(signal_rgb(p.background_sum)/2,s.environment_rgb,1e-7); require(p.guide_hits()==0 && p.identity[2]==0,"sky fabricated geometry"); }
        check(r.release_view(1));
    }
    std::puts("signals fresh/lobes/indirect/RR/emission/sky/raw-exact/Classic/ReSTIR/all epochs=pass");
}
void signal_geometry(memory::ResourceHandle heap,PathTraceRenderer& r,SubmissionQueue& q) {
    for(bool ortho:{false,true}) for(bool reuse:{false,true}) {
        Fixture f{heap}; f.quad(-.45f,.45f,0,{.7f,.4f,.2f}); const auto cpu=f.scene(); const auto scene=take(r.prepare_scene(q,cpu));
        const auto view=signal_view(f,7,5,ortho); auto s=settings(); s.environment_rgb={.2,.4,.8}; s.estimator=PathTraceEstimator::bsdf_only;
        PathTraceSchedule schedule; schedule.signals.enabled=true; schedule.target_spp=4; schedule.batch_spp=2; schedule.tile_edge=2;
        RestirDiSettings ris; ris.temporal.emplace(); if(reuse) s.estimator=PathTraceEstimator::mis;
        auto configure=[&](const RenderView& v) { if(reuse) check(r.configure_restir_view(q,1,scene,v,s,ris,schedule)); else check(r.configure_view(q,1,scene,v,s,schedule)); };
        configure(view); const auto image=finish_view(r,q,1); signal_recompose(image);
        unsigned misses=0,hits=0,mixed=0;
        for(std::uint32_t i=0;i<35;++i) {
            const auto& p=image.denoising_signals()[i]; unsigned count=0;
            for(unsigned sample=0;sample<4;++sample) {
                const auto ray=take(path_trace_camera_ray(view,i%7,i/7,{path_trace_sample(s.seed,i,sample,0,0),path_trace_sample(s.seed,i,sample,0,1)}));
                const auto hit=take(cpu.nearest(ray)); count+=unsigned(bool(hit));
                if(sample==2) { require(bool(hit)==bool(p.identity[2]&1),"representative guide is not batch first sample");
                    if(hit) { close_color(signal_rgb(p.position_view_depth),hit->position,1e-5); require(std::abs(p.position_view_depth[3]-1)<1e-6,"viewZ confused with ray distance");
                        require(std::abs(p.motion_distance[3]-hit->distance)<1e-5,"primary distance mismatch"); ++hits; } else ++misses; }
            }
            mixed+=count>0 && count<4; require(p.guide_hits()==count && !(p.identity[2]&2),"guide sample coverage or absent motion");
            close_color(signal_rgb(p.albedo_sum),Vec3f{.7f,.4f,.2f}.cast<double>()*count,1e-6);
            close_color(signal_rgb(p.normal_sum),Vec3d{0,0,double(count)},0);
        }
        require(hits && misses && mixed,"geometry fixture missed coverage boundary");
        const auto previous=r.completed_frame(1); schedule.signals.previous=previous.signal_reference(); schedule.target_spp=1; schedule.batch_spp=1;
        const auto moved=signal_view(f,7,5,ortho,.04); configure(moved); const auto motion=finish_view(r,q,1);
        signal_recompose(motion); require(motion.info().signals->source.sampling_first_epoch==(reuse ? 4u : 0u),"motion reset lost random epoch origin");
        require(motion.info().signals->previous->source.source_serial==previous.info().signals->source.source_serial,"motion reference lost consumed source identity");
        unsigned valid=0;
        for(std::uint32_t i=0;i<35;++i) { const auto& p=motion.denoising_signals()[i]; if(!(p.identity[2]&2)) continue;
            const Vec4d clip=schedule.signals.previous->world_to_clip*Vec4d{p.position_view_depth[0],p.position_view_depth[1],p.position_view_depth[2],1};
            const Vec2d expected={(clip.x()/clip.w()+1)*3.5-(i%7+path_trace_sample(s.seed,i,p.identity[1],0,0)),
                (clip.y()/clip.w()+1)*2.5-(i/7+path_trace_sample(s.seed,i,p.identity[1],0,1))};
            require((Vec2d{p.motion_distance[0],p.motion_distance[1]}-expected).norm()<2e-5,"motion sign/units/jitter mismatch"); ++valid;
        }
        require(valid>0,"motion reference produced no valid surface");
        const auto generation=take(r.status(1)).generation; schedule.signals.previous=r.completed_frame(1).signal_reference(); schedule.target_spp=2; schedule.exposure=1;
        configure(moved); require(take(r.status(1)).generation==generation,"reference/exposure reset raw");
        signal_recompose(finish_view(r,q,1)); check(r.release_view(1));
    }
    std::puts("signals perspective/orthographic/sampled-guides/miss/viewZ/motion/reference=pass");
}
void signal_textures(memory::ResourceHandle heap,PathTraceRenderer& r,SubmissionQueue& q) {
    for(unsigned mode=0;mode<3;++mode) for(bool reuse:{false,true}) {
        Fixture f{heap}; f.quad(-5,5,0,{.7f,.4f,.2f}); f.quad(-.22f,.22f,.5f,{1,1,1});
        for(unsigned i=2;i<4;++i) { add_texture(f,i,mode==2 ? Vec2f{.75f,.25f} : Vec2f{.25f,.25f}); if(mode==1) f.asset.materials[i].alpha_mode=AlphaMode::mask; }
        const auto cpu=f.scene(); const auto scene=take(r.prepare_scene(q,cpu)); const auto view=signal_view(f);
        auto s=settings(); s.surface_only=true; s.environment_rgb=Vec3d::Ones(); s.estimator=PathTraceEstimator::bsdf_only;
        PathTraceSchedule schedule; schedule.signals.enabled=true; schedule.target_spp=8; schedule.batch_spp=3; schedule.tile_edge=3;
        RestirDiSettings ris; ris.temporal.emplace(); ris.spatial.emplace(); if(reuse) s.estimator=PathTraceEstimator::mis;
        auto configure=[&](std::uint64_t id) { if(reuse) check(r.configure_restir_view(q,id,scene,view,s,ris,schedule)); else check(r.configure_view(q,id,scene,view,s,schedule)); };
        configure(1); const auto image=finish_view(r,q,1); signal_recompose(image);
        for(std::uint32_t i=0;i<35;++i) { Vec3d albedo=Vec3d::Zero();
            for(unsigned sample=0;sample<8;++sample) {
                const auto ray=take(path_trace_camera_ray(view,i%7,i/7,{path_trace_sample(s.seed,i,sample,0,0),path_trace_sample(s.seed,i,sample,0,1)}));
                const auto hit=take(cpu.nearest(ray)); require(bool(hit),"texture fixture missed floor");
                if(hit->primitive<2) albedo+=Vec3f{.7f,.4f,.2f}.cast<double>();
                else { require(mode!=1,"mask hole survived CPU guide"); albedo+=mode==2 ? Vec3d{0,1,0} : Vec3d{std::pow((128./255+.055)/1.055,2.4),0,0}; }
            }
            close_color(signal_rgb(image.denoising_signals()[i].albedo_sum)/8,albedo/8,.001);
        }
        schedule.signals.enabled=false; configure(2); compare_images(image,finish_view(r,q,2),0);
        check(r.release_view(1)); check(r.release_view(2));
    }
    std::puts("signals sampled texture/sRGB/mask/coverage/raw-exact=pass");
}
void signal_state(memory::ResourceHandle heap,PathTraceRenderer& r,SubmissionQueue& q) {
    Fixture f{heap}; f.quad(-5,5,0,{.7f,.4f,.2f}); const auto scene=take(r.prepare_scene(q,f.scene())); const auto view=signal_view(f);
    auto s=settings(); s.environment_rgb=Vec3d::Ones(); PathTraceSchedule schedule; schedule.signals.enabled=true; schedule.target_spp=2; schedule.batch_spp=2; schedule.tile_edge=2;
    check(r.configure_view(q,1,scene,view,s,schedule)); const auto original=finish_view(r,q,1); const auto old=r.completed_frame(1); const auto serial=old.info().signals->source.source_serial;
    schedule.target_spp=4; check(r.configure_view(q,1,scene,view,s,schedule)); take(r.submit_batch(q,1)); collect(r,q);
    require(r.completed_frame(1).info().signals->source.source_serial==serial,"partial tile advanced serial");
    check(r.cancel(1)); collect(r,q); require(take(r.status(1)).completed_spp==2,"cancel published raw");
    take(r.submit_batch(q,1)); collect(r,q); fail_submit=true; require(!r.submit_batch(q,1),"injected submit failure ignored");
    require(r.completed_frame(1).info().signals->source.source_serial==serial,"failed submit published signals");
    signal_recompose(finish_view(r,q,1)); require(r.completed_frame(1).info().signals->source.source_serial==serial+1,"failure consumed serial");
    const auto retained=complete(q,take(old.read_linear(q))); compare_images(retained,original,0);
    require(std::memcmp(retained.denoising_signals().data(),original.denoising_signals().data(),original.denoising_signals().size_bytes())==0,"failed batch corrupted old guides");
    schedule.signals.previous=r.completed_frame(1).signal_reference();
    require(!r.configure_view(q,2,scene,view,s,schedule),"another view motion reference accepted");
    schedule.signals.previous.reset(); check(r.configure_view(q,2,scene,view,s,schedule)); signal_recompose(finish_view(r,q,2));
    require(r.completed_frame(1).info().signals->source.view_id!=r.completed_frame(2).info().signals->source.view_id,"view identity shared");
    check(r.release_view(2));
    const auto generation=take(r.status(1)).generation; schedule.signals.enabled=false;
    check(r.configure_view(q,1,scene,view,s,schedule)); require(take(r.status(1)).generation>generation && !r.completed_frame(1),"signal disable retained mismatched raw");
    finish_view(r,q,1); schedule.signals.enabled=true; check(r.configure_view(q,1,scene,view,s,schedule)); require(!r.completed_frame(1),"mid-accumulation guides claimed old samples");
    signal_recompose(finish_view(r,q,1));
    schedule.target_spp=6; check(r.configure_view(q,1,scene,view,s,schedule)); take(r.submit_batch(q,1));
    schedule.signals.previous=r.completed_frame(1).signal_reference(); check(r.configure_view(q,1,scene,view,s,schedule)); collect(r,q);
    const auto frozen=finish_view(r,q,1); require(!frozen.info().signals->previous,"active signal reference was not frozen");
    const auto medium=r.configure_medium_view(q,1,scene,{},view,{},schedule); require(!medium && medium.error().code==ErrorCode::not_supported,"media signal capability not rejected");
    schedule.target_spp=8; check(r.configure_view(q,1,scene,view,s,schedule));
    std::stop_source stopped; take(r.submit_batch(q,1,stopped.get_token())); stopped.request_stop();
    check(r.release_view(1)); collect(r,q); require(!r.completed_frame(1),"retired in-flight signal frame published");
    compare_images(original,complete(q,take(old.read_linear(q))),0);
    // Real GPU budget: small valid old result remains when a larger signal candidate is rejected.
    { auto bounded=take(PathTraceRenderer::create(heap,q,DK_PATH_SHADER_DIR,{},1ull<<20)); const auto small=take(bounded.prepare_scene(q,f.scene()));
      schedule.signals.previous.reset(); schedule.target_spp=schedule.batch_spp=1;
      check(bounded.configure_view(q,1,small,view,s,schedule)); const auto before=finish_view(bounded,q,1); const auto identity=bounded.completed_frame(1).info().signals->source.source_serial;
      require(!bounded.configure_view(q,1,small,signal_view(f,64,64),s,schedule),"signal memory budget accepted oversized candidate");
      require(bounded.completed_frame(1).info().signals->source.source_serial==identity,"budget failure replaced completed input");
      compare_images(before,complete(q,take(bounded.completed_frame(1).read_linear(q))),0); check(bounded.release_view(1)); }
    std::puts("signals cancelled/failed/complete-only/immutable-owner/enable-reset/frozen-reference/views/budget=pass");
}
}

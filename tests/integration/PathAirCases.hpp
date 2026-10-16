#pragma once
namespace {
EnvironmentDescription air_environment(double extinction=2e-5,double scattering=1e-5) {
    EnvironmentDescription e; e.enabled=true; auto& a=e.atmosphere;
    a.atmosphere_height_m=10000; a.mie_scale_height_m=10000;
    a.absorption_center_height_m=5000; a.absorption_half_width_m=2500;
    a.rayleigh_scattering_per_m=a.absorption_extinction_per_m={0,0,0};
    a.mie_extinction_per_m={extinction,extinction,extinction}; a.mie_scattering_per_m={scattering,scattering,scattering};
    a.mie_anisotropy=0; a.ground_albedo={0,0,0}; return e;
}
VolumePathSettings air_settings(unsigned depth=1) {
    VolumePathSettings s; s.max_bounces=depth; s.rr_start=0; s.sun=false; return s;
}
PathTraceGpuImage air_image(PathTraceRenderer& r,SubmissionQueue& q,const PathTraceGpuScene& scene,const PathMediumSnapshot& m,
    VolumePathSettings s,unsigned samples,const PathTraceRay& ray,unsigned pixels=1) {
    std::vector<PathTraceRay> rays(pixels,ray);
    return complete(q,take(r.submit_air_rays(q,scene,m,rays,s,{0,samples})));
}
void air_numeric(memory::ResourceHandle heap,PathTraceRenderer& r,SubmissionQueue& q) {
    Fixture f{heap}; EnvironmentDescription e; auto m=take(PathMediumSnapshot::create(e));
    auto cpu=f.scene(); auto gpu=take(r.prepare_scene(q,cpu)); auto s=air_settings(0); s.environment_rgb={.2,.4,.8};
    auto image=air_image(r,q,gpu,m,s,4096,down);
    close_color(image.linear_rgb()[0],take(estimate_volume_path_reference(cpu,m,down,s,{0,4096})).mean,1e-7);
    require(image.info().air && image.info().settings.model_version==4 && image.info().settings.sampler_version==2,"air provenance missing");
    require(!image.primary_hits()[0] && gpu.submission().value()==0,"empty air requires geometry");
    // A tiny solar cone must not expand to sqrt(float epsilon) on the GPU.
    e.sun.angular_radius_rad=.00001; e.sun.direction={0,1,0}; f.document->set_environment(e).value();
    cpu=f.scene(); gpu=take(r.prepare_scene(q,cpu)); m=take(PathMediumSnapshot::create(e)); s.sun=true; s.environment_rgb.setZero();
    const PathTraceRay on{{0,1,0},{0,1,0}},off{{0,1,0},Vec3d{.00003,1,0}.normalized()};
    close_color(air_image(r,q,gpu,m,s,16,off).linear_rgb()[0],Vec3d::Zero(),0);
    auto solar=air_image(r,q,gpu,m,s,4096,on); auto solar_cpu=take(estimate_volume_path_reference(cpu,m,on,s,{0,4096}));
    require((solar.linear_rgb()[0]-solar_cpu.mean).cwiseAbs().maxCoeff()/solar_cpu.mean.maxCoeff()<2e-6,"finite solar disk mismatch");
    // Near-ground height and atmospheric extent survive Earth-sized float coordinates.
    e=air_environment(0,0); f.document->set_environment(e).value(); cpu=f.scene(); gpu=take(r.prepare_scene(q,cpu)); m=take(PathMediumSnapshot::create(e));
    s=air_settings(0); s.environment_rgb=Vec3d::Ones();
    const PathTraceRay ground{{0,1.8,0},{0,-1,0},0,.1},escape{{0,1.8,0},{0,1,0},0,.1};
    close_color(air_image(r,q,gpu,m,s,128,ground).linear_rgb()[0],Vec3d::Zero(),0);
    close_color(air_image(r,q,gpu,m,s,4096,escape).linear_rgb()[0],take(estimate_volume_path_reference(cpu,m,escape,s,{0,4096})).mean,1e-7);
    // Tangential and distant sky paths, full supported planet radius, opaque night-side boundary.
    for(double radius:{1.,1e7}) {
        e.atmosphere.ground_radius_m=radius; e.atmosphere.planet_center_m={0,-radius,0};
        f.document->set_environment(e).value(); cpu=f.scene(); gpu=take(r.prepare_scene(q,cpu)); m=take(PathMediumSnapshot::create(e));
        const std::array<PathTraceRay,3> boundary{{{{0,1.8,0},{1,0,0}},{{0,20000,0},{0,1,0}},{{0,1.8,0},{0,-1,0}}}};
        for(const auto& ray:boundary) close_color(air_image(r,q,gpu,m,s,128,ray).linear_rgb()[0],
            take(estimate_volume_path_reference(cpu,m,ray,s,{0,128})).mean,1e-7);
    }
    e=air_environment(1e-38,0); f.document->set_environment(e).value(); cpu=f.scene(); gpu=take(r.prepare_scene(q,cpu)); m=take(PathMediumSnapshot::create(e));
    close_color(air_image(r,q,gpu,m,s,4096,escape).linear_rgb()[0],take(estimate_volume_path_reference(cpu,m,escape,s,{0,4096})).mean,1e-7);
    // Nonempty geometry, alpha masking, finite sun and surface/reflection paths share the air integrator.
    e=air_environment(2e-5,1e-5); e.atmosphere.planet_center_m[1]-=100; e.sun.direction={0,0,1}; e.sun.angular_radius_rad=.05;
    f.document->set_environment(e).value(); f.quad(-10,10,0,{.5f,.6f,.7f}); f.quad(-2,2,2,{0,0,0},{0,0,0},true);
    for(unsigned i=2;i<f.asset.materials.size();++i) { f.asset.materials[i].alpha_mode=AlphaMode::mask; f.asset.materials[i].base_color.w()=0; }
    m=take(PathMediumSnapshot::create(e)); s=air_settings(3); s.sun=true; s.environment_rgb=Vec3d::Constant(.1);
    for(bool ggx:{false,true}) {
        if(ggx) for(auto& material:f.asset.materials) { material.metallic=.8f; material.roughness=.25f; }
        const std::array assets{&f.asset}; cpu=take(PathTraceScene::create(heap,f.source(),assets,{},ggx ? PathTraceMaterialModel::metallic_roughness : PathTraceMaterialModel::lambert));
        gpu=take(r.prepare_scene(q,cpu)); const auto reference=take(estimate_volume_path_reference(cpu,m,down,s,{0,16384}));
        image=air_image(r,q,gpu,m,s,16384,down); close_color(image.linear_rgb()[0],reference.mean,.002);
        require(image.primary_hits()[0].has_value(),"air surface primary diagnostic missing");
    }
    std::puts("air vacuum/finite sun/near-ground/far-clip/Lambert/GGX/mask/CPU=pass");
}
struct AirPopulation { Vec3d mean=Vec3d::Zero(),error=Vec3d::Zero(); double mse=0,cpu_rmse=0; };
AirPopulation air_population(PathTraceRenderer& r,SubmissionQueue& q,const PathTraceGpuScene& scene,const PathMediumSnapshot& m,
    const PathTraceRay& ray,VolumePathSettings s,unsigned samples,const Vec3d& expected,unsigned pixels=1,bool cpu=false) {
    AirPopulation p; std::array<Vec3d,8> values; double difference=0,energy=0;
    for(unsigned seed=0;seed<8;++seed) {
        s.seed=101+7919*seed; const auto image=air_image(r,q,scene,m,s,samples,ray,pixels); values[seed].setZero();
        for(unsigned pixel=0;pixel<pixels;++pixel) {
            values[seed]+=image.linear_rgb()[pixel]/pixels;
            if(cpu) {
                const auto reference=take(estimate_volume_path_reference(scene.source(),m,ray,s,{0,samples},pixel));
                difference+=(image.linear_rgb()[pixel]-reference.mean).squaredNorm(); energy+=reference.mean.squaredNorm();
            }
        }
        p.mean+=values[seed]/8; p.mse+=(values[seed]-expected).squaredNorm()/24;
    }
    for(const auto& value:values) p.error+=(value-p.mean).cwiseAbs2()/56;
    p.error=p.error.cwiseSqrt(); p.cpu_rmse=std::sqrt(difference/std::max(energy,1e-20)); return p;
}
void air_statistical_check(const AirPopulation& p,const Vec3d& expected) {
    std::printf("air mean=(%.9g %.9g %.9g) expected=(%.9g %.9g %.9g) stderr=(%.6g %.6g %.6g) MSE=%.9g CPU nRMSE=%.9g\n",
        p.mean.x(),p.mean.y(),p.mean.z(),expected.x(),expected.y(),expected.z(),p.error.x(),p.error.y(),p.error.z(),p.mse,p.cpu_rmse);
    for(unsigned c=0;c<3;++c) {
        require(std::abs(p.mean[c]-expected[c])<=.02*std::abs(expected[c])+1e-4,"air mean outside 2% reference");
        require(std::abs(p.mean[c]-expected[c])<=5*p.error[c]+1e-4,"air mean outside 5 standard errors");
    }
}
void air_statistics(memory::ResourceHandle heap,PathTraceRenderer& r,SubmissionQueue& q) {
    Fixture f{heap}; auto e=air_environment(0,0); e.atmosphere.mie_extinction_per_m={2e-5,4e-5,6e-5}; f.document->set_environment(e).value();
    auto cpu=f.scene(); auto gpu=take(r.prepare_scene(q,cpu)); auto m=take(PathMediumSnapshot::create(e)); auto s=air_settings(0); s.environment_rgb={.3,.5,.8};
    const PathTraceRay up{{0,1,0},{0,1,0},.1,10}; Vec3d expected;
    for(int c=0;c<3;++c) expected[c]=s.environment_rgb[c]*std::exp(-e.atmosphere.mie_extinction_per_m[c]*10000*(std::exp(-.0001)-std::exp(-1.)));
    const auto transmission=take(m.integrate({up.origin,up.direction,10000}));
    close_color(transmission.transmittance.cwiseProduct(s.environment_rgb),expected,1e-4);
    auto absorption=air_population(r,q,gpu,m,up,s,65536,expected,2,true); air_statistical_check(absorption,expected);
    require(absorption.cpu_rmse<1e-4,"absorption CPU/GPU tracking disagreement");
    // Independently integrated single-scatter: equal extinction and isotropic scattering,
    // Overhead narrow sun, top-to-ground view: both view and sun attenuation cover top-to-vertex.
    e=air_environment(5e-5,5e-5); e.sun.angular_radius_rad=.001; f.document->set_environment(e).value();
    cpu=f.scene(); gpu=take(r.prepare_scene(q,cpu)); m=take(PathMediumSnapshot::create(e)); s=air_settings(1); s.sun=true;
    const double tau=5e-5*10000*(1-std::exp(-1.));
    expected=Vec3d::Constant((1-std::exp(-2*tau))/(8*std::numbers::pi));
    const PathTraceRay into{{0,10000,0},{0,-1,0}};
    const auto n=air_population(r,q,gpu,m,into,s,16384,expected,16);
    const auto four=air_population(r,q,gpu,m,into,s,65536,expected,16);
    air_statistical_check(n,expected); air_statistical_check(four,expected);
    require(four.mse<n.mse*.6,"air N/4N variance did not decrease");
    // Mixed Rayleigh/HG multiple scattering under a low sun; compare independent CPU paths.
    e.atmosphere.rayleigh_scattering_per_m={1e-5,2e-5,4e-5}; e.atmosphere.mie_anisotropy=.7;
    e.sun.direction={std::sqrt(1-.08*.08),.08,0}; f.document->set_environment(e).value();
    cpu=f.scene(); gpu=take(r.prepare_scene(q,cpu)); m=take(PathMediumSnapshot::create(e)); s=air_settings(6); s.sun=true; s.rr_start=3;
    Vec3d oracle=Vec3d::Zero();
    for(unsigned seed=0;seed<8;++seed) { s.seed=101+7919*seed; for(unsigned pixel=0;pixel<4;++pixel)
        oracle+=take(estimate_volume_path_reference(cpu,m,into,s,{0,16384},pixel)).mean/32; }
    auto sunset=air_population(r,q,gpu,m,into,s,16384,oracle,4,true); air_statistical_check(sunset,oracle);
    require(sunset.cpu_rmse<.02,"low-sun multiple scattering CPU/GPU nRMSE exceeds 2%");
    std::puts("air RGB absorption/analytic transmittance/single scatter/N4N/low sun/multiple scatter/RR=pass");
}
void air_progressive(memory::ResourceHandle heap,PathTraceRenderer& r,SubmissionQueue& q) {
    Fixture f{heap}; auto e=air_environment(); e.atmosphere.planet_center_m[1]-=100; f.document->set_environment(e).value();
    f.quad(-.32f,.32f,0,{.6f,.4f,.2f}); f.quad(-.1f,.1f,.6f,{0,0,0},{2,3,4},true);
    auto cpu=f.scene(); auto gpu=take(r.prepare_scene(q,cpu)); auto m=take(PathMediumSnapshot::create(e)); auto view=f.view(f.source(),7);
    auto s=air_settings(4); s.environment_rgb={.2,.4,.6}; s.sun=true; s.seed=723;
    auto once=complete(q,take(r.submit_air_view(q,gpu,m,view,s,{0,19})));
    const auto reference=take(render_volume_path_reference(heap,cpu,m,view,s,{0,19}));
    for(std::size_t i=0;i<reference.linear_rgb().size();++i) close_color(once.linear_rgb()[i],reference.linear_rgb()[i],2e-4);
    PathTraceSchedule schedule; schedule.target_spp=19;
    for(unsigned batch:{3,7,16}) {
        schedule.batch_spp=batch; schedule.tile_edge=batch==3 ? 4 : 2;
        check(r.configure_air_view(q,11,gpu,m,view,s,schedule)); compare_images(finish_view(r,q,11),once,0); check(r.release_view(11));
    }
    check(r.configure_air_view(q,11,gpu,m,view,s,schedule)); auto old=finish_view(r,q,11); auto held=r.completed_frame(11);
    auto generation=take(r.status(11)).generation; schedule.exposure=2; schedule.target_spp=20; ++s.max_tracking_steps;
    check(r.configure_air_view(q,11,gpu,m,view,s,schedule)); require(take(r.status(11)).generation==generation,"schedule/budget reset air history");
    take(r.submit_batch(q,11));
    const auto submitted=q.stats().submitted,geometry=cpu.geometry_fingerprint();
    e.sun.direction={1,0,0}; e.atmosphere.mie_anisotropy=.5; f.document->set_environment(e).value();
    auto changed=f.scene(); auto next=take(r.prepare_scene(q,changed));
    require(changed.geometry_fingerprint()==geometry && changed.fingerprint()!=cpu.fingerprint(),"environment/geometry identity mixed");
    require(q.stats().submitted==submitted && next.submission().value()==gpu.submission().value(),"environment change rebuilt AS");
    m=take(PathMediumSnapshot::create(e)); view=f.view(f.source(),7);
    check(r.configure_air_view(q,11,next,m,view,s,schedule)); require(!collect(r,q),"late old atmosphere published");
    require(take(r.status(11)).generation!=generation && take(r.status(11)).completed_spp==0,"air change did not reset history");
    auto updated=finish_view(r,q,11); require(updated.info().air->environment==e && updated.info().revision==changed.revision(),"air result provenance stale");
    compare_images(complete(q,take(held.read_linear(q))),old,0);
    f.asset.materials[0].base_color.x()=.9f; auto material=f.scene(); const auto before=q.stats().submitted;
    auto rebuilt=take(r.prepare_scene(q,material)); require(material.geometry_fingerprint()!=geometry && q.stats().submitted>before,"material change reused stale geometry");
    require(take(q.wait(rebuilt.submission())),"rebuilt scene not ready"); check(r.release_view(11));
    std::puts("air progressive exact 3/7/16 batches/tiles/history/reset/late completion/AS reuse/provenance=pass");
}
void air_failure(memory::ResourceHandle heap,PathTraceRenderer& r,SubmissionQueue& q) {
    Fixture f{heap}; auto e=air_environment(); e.atmosphere.planet_center_m[1]-=100; f.document->set_environment(e).value();
    auto cpu=f.scene(); auto gpu=take(r.prepare_scene(q,cpu)); auto m=take(PathMediumSnapshot::create(e)); auto view=f.view(f.source(),4);
    auto s=air_settings(3); s.environment_rgb=Vec3d::Ones(); PathTraceSchedule schedule; schedule.target_spp=2; schedule.batch_spp=2;
    check(r.configure_air_view(q,1,gpu,m,view,s,schedule)); auto old=finish_view(r,q,1); auto held=r.completed_frame(1); const auto generation=take(r.status(1)).generation;
    schedule.target_spp=4; auto low=s; low.max_tracking_steps=1;
    check(r.configure_air_view(q,1,gpu,m,view,low,schedule)); take(r.submit_batch(q,1)); q.device().logical_device().waitIdle();
    auto failed=r.poll(q); require(!failed && failed.error().message.find("budget")!=std::string::npos,"GPU air budget did not fail explicitly");
    require(take(r.status(1)).generation==generation && take(r.status(1)).completed_spp==2 && !take(r.status(1)).pending,"failed air batch advanced history");
    compare_images(complete(q,take(r.completed_frame(1).read_linear(q))),old,0);
    const PathTraceRay up{{0,1,0},{0,1,0}};
    auto direct=take(r.submit_air_rays(q,gpu,m,std::span{&up,1},low,{0,128})); require(take(q.wait(direct.submission())),"budget frame wait");
    require(!direct.read(q),"one-shot partial air image published");
    auto path_limit=s; path_limit.max_steps_per_path=1;
    auto limited=take(r.submit_air_rays(q,gpu,m,std::span{&up,1},path_limit,{0,128})); require(take(q.wait(limited.submission())),"per-path frame wait");
    const auto limited_result=limited.read(q);
    require(!limited_result && limited_result.error().message.find("budget")!=std::string::npos,"per-path tracking budget ignored");
    check(r.configure_air_view(q,1,gpu,m,view,s,schedule)); fail_submit=true; require(!r.submit_batch(q,1),"injected submit failure ignored");
    require(take(r.status(1)).completed_spp==2,"failed submit advanced history");
    std::stop_source stop; take(r.submit_batch(q,1,stop.get_token())); stop.request_stop(); require(!collect(r,q),"cancelled air tile published");
    auto recovered=finish_view(r,q,1); require(recovered.info().samples.count==4,"air failure recovery lost range");
    const auto submitted=q.stats().submitted;
    auto invalid=s; invalid.sampler_version=1; require(!r.configure_air_view(q,1,gpu,m,view,invalid,schedule),"invalid sampler admitted");
    const PathTraceRay underground{{0,-101,0},{0,1,0}};
    require(!r.submit_air_rays(q,gpu,m,std::span{&underground,1},s,{0,1}),"underground air camera admitted");
    auto mismatched=e; mismatched.atmosphere.mie_anisotropy=.3;
    require(!r.submit_air_view(q,gpu,take(PathMediumSnapshot::create(mismatched)),view,s,{0,1}),"mismatched medium admitted");
    e.clouds.enabled=true; e.clouds.density=0; f.document->set_environment(e).value(); auto clouds=take(r.prepare_scene(q,f.scene()));
    require(!r.submit_air_rays(q,clouds,take(PathMediumSnapshot::create(e)),std::span{&up,1},s,{0,1}),"GPU cloud capability claimed");
    require(q.stats().submitted==submitted,"rejected air request submitted work");
    compare_images(complete(q,take(held.read_linear(q))),old,0); check(r.release_view(1));
    std::puts("air tracking exhaustion/no partial publication/submit failure/cancel/recovery/admission=pass");
}
void air_resources(memory::ResourceHandle heap,SubmissionQueue& q) {
    auto r=take(PathTraceRenderer::create(heap,q,DK_PATH_SHADER_DIR,{},1u<<20)); const auto baseline=r.memory_stats(q).owned.current;
    {
        Fixture f{heap}; f.quad(-2,2,0,{.5f,.5f,.5f});
        TextureData texture; texture.id=AssetId::generate().value(); texture.width=texture.height=2; texture.rgba8.resize(16,std::byte{255});
        for(unsigned i=0;i<2;++i) { f.asset.materials[i].base_color_texture=texture.id; f.asset.mesh.primitives[i].texcoords={{0,0},{1,0},{0,1}}; }
        f.asset.textures.push_back(std::move(texture)); auto first=take(r.prepare_scene(q,f.scene())); require(take(q.wait(first.submission())),"texture build failed");
        const auto owned=r.memory_stats(q).owned.current,submitted=q.stats().submitted;
        auto e=air_environment(); e.atmosphere.planet_center_m[1]-=100; f.document->set_environment(e).value();
        auto second=take(r.prepare_scene(q,f.scene()));
        require(q.stats().submitted==submitted && r.memory_stats(q).owned.current==owned,"environment duplicated texture/AS allocation");
        auto m=take(PathMediumSnapshot::create(e)); auto s=air_settings(1); s.environment_rgb=Vec3d::Ones();
        auto textured=air_image(r,q,second,m,s,1024,down); require(textured.info().texture_count==1 && textured.primary_hits()[0],"textured air path not exercised");
        f.asset.textures[0].rgba8[0]=std::byte{0}; auto changed=f.scene(); auto third=take(r.prepare_scene(q,changed));
        require(q.stats().submitted>submitted && third.source().geometry_fingerprint()!=second.source().geometry_fingerprint(),"texture change reused stale bindings");
        require(take(q.wait(third.submission())),"texture rebuild failed");
        const auto before=q.stats().submitted; f.asset.mesh.primitives[0].positions[0].x()+=.25f; fail_submit=true;
        require(!r.prepare_scene(q,f.scene()) && q.stats().submitted==before,"failed geometry candidate submitted");
        auto recovered=take(r.prepare_scene(q,changed)); require(q.stats().submitted==before,"failed candidate replaced reusable geometry");
    }
    require(r.memory_stats(q).owned.current==baseline,"weak geometry cache retained GPU resource charges");
    Fixture empty{heap}; auto e=air_environment(0,0); e.atmosphere.planet_center_m[1]-=100; empty.document->set_environment(e).value();
    auto scene=take(r.prepare_scene(q,empty.scene())); auto view=empty.view(empty.source(),48); auto m=take(PathMediumSnapshot::create(e));
    auto s=air_settings(0); s.environment_rgb=Vec3d::Ones(); PathTraceSchedule schedule; schedule.target_spp=1;
    check(r.configure_air_view(q,1,scene,m,view,s,schedule)); advance_view(r,q,1); auto held=r.completed_frame(1);
    check(r.reset(1)); advance_view(r,q,1); auto second=r.completed_frame(1);
    check(r.reset(1)); advance_view(r,q,1); schedule.target_spp=2;
    check(r.configure_air_view(q,1,scene,m,view,s,schedule)); require(!r.submit_batch(q,1),"air memory budget ignored");
    require(take(r.status(1)).completed_spp==1,"memory failure published air frame");
    held={}; second={}; advance_view(r,q,1); require(take(r.status(1)).completed_spp==2,"air memory recovery failed");
    auto stats=r.memory_stats(q); require(stats.owned.peak<=stats.owned.limit,"air resource budget overshot"); check(r.release_view(1));
    require(r.memory_stats(q).owned.current==baseline,"air resources retained after release");
    std::printf("air 1MiB resource limit peak=%llu texture/AS reuse/invalidation/failure/weak cache/external owners=pass\n",stats.owned.peak);
}
}

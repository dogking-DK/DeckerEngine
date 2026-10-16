#pragma once
namespace {
EnvironmentDescription cloud_environment() {
    auto e=air_environment(0,0); e.enabled=false; e.clouds.enabled=true; e.clouds.coverage=1;
    e.clouds.weather_resolution=e.clouds.base_resolution=e.clouds.detail_resolution=16;
    return e;
}
PathTraceGpuImage cloud_image(PathTraceRenderer& r,SubmissionQueue& q,const PathTraceGpuScene& scene,const PathMediumSnapshot& m,
    VolumePathSettings s,unsigned samples,const PathTraceRay& ray,unsigned pixels=1) {
    std::vector<PathTraceRay> rays(pixels,ray);
    auto result=complete(q,take(r.submit_medium_rays(q,scene,m,rays,s,{0,samples})));
    require(result.info().medium && result.info().medium->field_hash==m.field_hash(),"cloud provenance missing"); return result;
}
struct CloudPopulation {
    Vec3d mean=Vec3d::Zero(),reference=Vec3d::Zero(),error=Vec3d::Zero(),reference_error=Vec3d::Zero();
    double mse=0,rmse=0; std::uint64_t multiple=0;
};
CloudPopulation cloud_population(PathTraceRenderer& r,SubmissionQueue& q,const PathTraceGpuScene& scene,const PathMediumSnapshot& m,
    const PathTraceRay& ray,VolumePathSettings s,unsigned samples,unsigned pixels,const Vec3d* expected=nullptr) {
    CloudPopulation result; std::array<Vec3d,8> values,reference; double difference=0,energy=0;
    for(unsigned seed=0;seed<8;++seed) {
        s.seed=101+7919*seed; auto image=cloud_image(r,q,scene,m,s,samples,ray,pixels); values[seed].setZero(); reference[seed].setZero();
        for(unsigned pixel=0;pixel<pixels;++pixel) {
            const auto value=image.linear_rgb()[pixel]; values[seed]+=value/double(pixels);
            Vec3d target;
            if(expected) target=*expected;
            else {
                auto cpu=take(estimate_volume_path_reference(scene.source(),m,ray,s,{0,samples},pixel)); target=cpu.mean;
                result.multiple+=cpu.statistics.multiple_scatter_paths;
            }
            reference[seed]+=target/double(pixels); difference+=(value-target).squaredNorm(); energy+=target.squaredNorm();
        }
        result.mean+=values[seed]/8; result.reference+=reference[seed]/8;
        result.mse+=(values[seed]-reference[seed]).squaredNorm()/24;
    }
    for(unsigned i=0;i<8;++i) { result.error+=(values[i]-result.mean).cwiseAbs2()/56; result.reference_error+=(reference[i]-result.reference).cwiseAbs2()/56; }
    result.error=result.error.cwiseSqrt(); result.reference_error=result.reference_error.cwiseSqrt();
    result.rmse=std::sqrt(difference/std::max(energy,1e-20));
    std::printf("cloud spp=%u mean=(%.8g %.8g %.8g) reference=(%.8g %.8g %.8g) nRMSE=%.8g MSE=%.8g multiple=%llu\n",
        samples,result.mean.x(),result.mean.y(),result.mean.z(),result.reference.x(),result.reference.y(),result.reference.z(),result.rmse,result.mse,result.multiple);
    return result;
}
void cloud_check(const CloudPopulation& p,bool paired=true) {
    for(unsigned c=0;c<3;++c) {
        require(std::abs(p.mean[c]-p.reference[c])<=.02*std::abs(p.reference[c])+1e-4,"cloud mean outside 2% reference");
        require(std::abs(p.mean[c]-p.reference[c])<=5*std::hypot(p.error[c],p.reference_error[c])+1e-4,"cloud mean outside 5 joint standard errors");
    }
    if(paired) require(p.rmse<=.02,"cloud CPU/GPU nRMSE exceeds 2%");
}
void cloud_numeric(memory::ResourceHandle heap,PathTraceRenderer& r,SubmissionQueue& q) {
    Fixture f{heap}; auto e=cloud_environment(); CloudFieldCache cache; auto fields=take(cache.prepare(e.clouds));
    PathMediumSettings ms; ms.cloud_albedo=0; ms.time_seconds=17;
    auto s=air_settings(0); s.environment_rgb=Vec3d::Ones();
    for(unsigned mode=0;mode<2;++mode) {
        if(mode) { e.atmosphere.ground_radius_m=1000; e.atmosphere.planet_center_m={0,0,0}; e.clouds.extinction_per_m=.0002; }
        f.document->set_environment(e).value(); auto cpu=f.scene(); auto gpu=take(r.prepare_scene(q,cpu)); auto m=take(PathMediumSnapshot::create(e,ms,fields));
        std::vector<PathTraceRay> rays;
        if(!mode) rays={{{700,1,-500},{0,1,0},.1,10},{{700,2000,-500},{0,1,0}},{{700,6000,-500},{0,1,0}}};
        else rays={{{-6000,2000,0},{1,0,0}},{{-6000,5000,0},{1,0,0}},{{-6000,4999,0},{1,0,0}}};
        for(const auto& ray:rays) {
            auto reference=take(m.integrate({ray.origin,ray.direction,20000},{8192,40960}));
            close_color(reference.transmittance,take(m.integrate({ray.origin,ray.direction,20000},{4096,40960})).transmittance,1e-4);
            auto p=cloud_population(r,q,gpu,m,ray,s,65536,2,&reference.transmittance); cloud_check(p,false);
            cloud_check(cloud_population(r,q,gpu,m,ray,s,8192,1));
            if(mode && ray.origin.y()==2000) require(take(m.intervals({ray.origin,ray.direction,20000})).count==2,"double cloud shell fixture missing");
        }
    }
    // Zero coverage/density/extinction requires no field and agrees exactly with the air entry.
    e=air_environment(); e.clouds=cloud_environment().clouds;
    const PathTraceRay up{{700,1,-500},{0,1,0}}; s=air_settings(3); s.environment_rgb=Vec3d::Ones();
    for(unsigned zero=0;zero<3;++zero) {
        auto empty=e; if(zero==0) empty.clouds.coverage=0; if(zero==1) empty.clouds.density=0; if(zero==2) empty.clouds.extinction_per_m=0;
        f.document->set_environment(empty).value(); auto m=take(PathMediumSnapshot::create(empty)); require(!m.field(),"zero cloud retained fields");
        auto scene=take(r.prepare_scene(q,f.scene())); auto actual=cloud_image(r,q,scene,m,s,4096,up);
        empty.clouds.enabled=false; f.document->set_environment(empty).value(); auto air=take(r.prepare_scene(q,f.scene()));
        compare_images(actual,air_image(r,q,air,take(PathMediumSnapshot::create(empty)),s,4096,up),0);
    }
    // Large signed wind time is reduced in double before GPU upload; periodic fields stay fixed.
    e=cloud_environment(); e.clouds.wind_velocity_mps={10,0}; f.document->set_environment(e).value(); auto gpu=take(r.prepare_scene(q,f.scene()));
    ms.time_seconds=-1e9; ms.cloud_albedo=.9; auto m=take(PathMediumSnapshot::create(e,ms,fields));
    cloud_check(cloud_population(r,q,gpu,m,up,s,8192,2));
    auto original=cloud_image(r,q,gpu,m,s,1024,up); ms.time_seconds+=10000; // 100000m is a common period of all three fields.
    compare_images(original,cloud_image(r,q,gpu,take(PathMediumSnapshot::create(e,ms,fields)),s,1024,up),0);
    // Every sampled coefficient must remain below the analytical, world-space majorant.
    for(int x=-8;x<=8;++x) for(unsigned y=0;y<=40;++y) {
        const auto c=take(m.sample({x*731.,y*125.,-x*593.}));
        require((c.extinction.array()<=m.extinction_majorant().array()).all(),"world cloud majorant underestimated");
    }
    std::puts("cloud below/inside/above/double shell/tangent/zero/fixed wind/majorant=pass");
}
void cloud_statistics(memory::ResourceHandle heap,PathTraceRenderer& r,SubmissionQueue& q) {
    Fixture f{heap}; auto e=cloud_environment(); CloudFieldCache cache; auto fields=take(cache.prepare(e.clouds));
    PathMediumSettings ms; ms.time_seconds=17; ms.cloud_albedo=0; f.document->set_environment(e).value();
    auto gpu=take(r.prepare_scene(q,f.scene())); auto m=take(PathMediumSnapshot::create(e,ms,fields));
    const PathTraceRay up{{700,1,-500},{0,1,0}}; auto s=air_settings(0); s.environment_rgb=Vec3d::Ones();
    const auto expected=take(m.integrate({up.origin,up.direction,20000},{8192,40960})).transmittance;
    auto n=cloud_population(r,q,gpu,m,up,s,16384,4,&expected),four=cloud_population(r,q,gpu,m,up,s,65536,4,&expected);
    cloud_check(n,false); cloud_check(four,false); require(four.mse<n.mse*.6,"cloud N/4N error did not decrease");
    // Actual repeated cloud events conserve energy in a white furnace; no low-order boost.
    e.atmosphere.ground_albedo={1,1,1}; f.document->set_environment(e).value(); gpu=take(r.prepare_scene(q,f.scene()));
    ms.cloud_albedo=1; m=take(PathMediumSnapshot::create(e,ms,fields)); s=air_settings(32); s.environment_rgb=Vec3d::Ones();
    const auto furnace=cloud_population(r,q,gpu,m,up,s,16384,2); cloud_check(furnace);
    require(furnace.multiple>100,"cloud multiple scattering not exercised"); close_color(furnace.mean,Vec3d::Ones(),.02);
    // Mixed Rayleigh/Mie/cloud phase, low sun, independent NEE/continuation and RR paths.
    e.enabled=true; e.atmosphere.rayleigh_scattering_per_m={2e-6,3e-6,4e-6};
    e.atmosphere.mie_scattering_per_m=e.atmosphere.mie_extinction_per_m={1e-5,1e-5,1e-5}; e.atmosphere.mie_anisotropy=.7;
    e.sun.direction={.8,.6,0}; e.sun.angular_radius_rad=.1; e.atmosphere.ground_albedo={.3,.3,.3};
    f.document->set_environment(e).value(); gpu=take(r.prepare_scene(q,f.scene())); ms.cloud_albedo=.9;
    m=take(PathMediumSnapshot::create(e,ms,fields)); s=air_settings(6); s.sun=true; s.environment_rgb=Vec3d::Constant(.2);
    const PathTraceRay inside{{700,2000,-500},{0,1,0}};
    cloud_check(cloud_population(r,q,gpu,m,inside,s,16384,2));
    s.rr_start=3; cloud_check(cloud_population(r,q,gpu,m,inside,s,16384,2));
    s.estimator=PathTraceEstimator::bsdf_only; cloud_check(cloud_population(r,q,gpu,m,inside,s,16384,2));
    std::puts("cloud N4N/white furnace/multiple scatter/mixed phase/MIS/continuation/RR=pass");
}
void cloud_surface(memory::ResourceHandle heap,PathTraceRenderer& r,SubmissionQueue& q) {
    Fixture f{heap}; auto e=cloud_environment(); e.sun.angular_radius_rad=.0001; f.document->set_environment(e).value();
    f.quad(-10000,10000,50,{.5f,.5f,.5f});
    for(auto& primitive:f.asset.mesh.primitives) for(auto& p:primitive.positions) p={p.x(),p.z(),-p.y()};
    TextureData texture; texture.id=AssetId::generate().value(); texture.width=texture.height=2; texture.rgba8.resize(16,std::byte{255});
    for(unsigned i=0;i<2;++i) { f.asset.materials[i].base_color_texture=texture.id; f.asset.mesh.primitives[i].texcoords={{0,0},{1,0},{0,1}}; }
    f.asset.textures.push_back(std::move(texture));
    CloudFieldCache cache; auto fields=take(cache.prepare(e.clouds)); PathMediumSettings ms; ms.cloud_albedo=0; ms.time_seconds=17;
    auto m=take(PathMediumSnapshot::create(e,ms,fields)); auto gpu=take(r.prepare_scene(q,f.scene()));
    auto s=air_settings(1); s.sun=true; const PathTraceRay downCloud{{700,60,-500},{0,-1,0}};
    const Vec3d shadow=take(m.integrate({{700,50,-500},{0,1,0},20000},{8192,40960})).transmittance*(.5/std::numbers::pi);
    require(shadow.x()<.99*.5/std::numbers::pi,"cloud shadow fixture has no attenuation");
    cloud_check(cloud_population(r,q,gpu,m,downCloud,s,65536,4,&shadow),false);
    cloud_check(cloud_population(r,q,gpu,m,downCloud,s,16384,2));
    // A ceiling blocks the scene sun before the cloud shell; a transparent mask admits it again.
    const auto first=f.asset.mesh.primitives.size(); f.quad(-10000,10000,1000,{0,0,0},{0,0,0},true);
    for(std::size_t i=first;i<f.asset.mesh.primitives.size();++i) for(auto& p:f.asset.mesh.primitives[i].positions) p={p.x(),p.z(),-p.y()};
    gpu=take(r.prepare_scene(q,f.scene())); close_color(cloud_image(r,q,gpu,m,s,4096,downCloud).linear_rgb()[0],Vec3d::Zero(),0);
    for(std::size_t i=first;i<f.asset.materials.size();++i) { f.asset.materials[i].alpha_mode=AlphaMode::mask; f.asset.materials[i].base_color.w()=0; }
    gpu=take(r.prepare_scene(q,f.scene())); cloud_check(cloud_population(r,q,gpu,m,downCloud,s,16384,2));
    // Indirect cloud radiance reflected by a metal/roughness surface, with real cloud scattering.
    for(auto& material:f.asset.materials) { material.metallic=.6f; material.roughness=.4f; }
    const std::array assets{&f.asset}; auto cpu=take(PathTraceScene::create(heap,f.source(),assets,{},PathTraceMaterialModel::metallic_roughness));
    gpu=take(r.prepare_scene(q,cpu)); ms.cloud_albedo=.9; m=take(PathMediumSnapshot::create(e,ms,fields));
    s=air_settings(4); s.environment_rgb={.3,.4,.5}; cloud_check(cloud_population(r,q,gpu,m,downCloud,s,16384,2));
    Fixture volume{heap}; volume.document->set_environment(e).value(); volume.quad(-1000000,1000000,4500,{0,0,0},{0,0,0},true);
    for(auto& primitive:volume.asset.mesh.primitives) for(auto& p:primitive.positions) p={p.x(),p.z(),-p.y()};
    auto ceiling=take(r.prepare_scene(q,volume.scene())); const PathTraceRay within{{700,2000,-500},{1,0,0}};
    s=air_settings(1); s.sun=true; close_color(cloud_image(r,q,ceiling,m,s,8192,within).linear_rgb()[0],Vec3d::Zero(),0);
    for(auto& material:volume.asset.materials) { material.alpha_mode=AlphaMode::mask; material.base_color.w()=0; }
    auto transparent=take(r.prepare_scene(q,volume.scene())); auto lit=cloud_population(r,q,transparent,m,within,s,16384,2); cloud_check(lit);
    require(lit.mean.maxCoeff()>1e-3,"cloud scattering geometry shadow fixture unlit");
    std::puts("cloud solar shadow/geometry occlusion at surface and volume/mask/GGX reflection=pass");
}
void cloud_progressive(memory::ResourceHandle heap,PathTraceRenderer& r,SubmissionQueue& q) {
    Fixture f{heap}; auto e=cloud_environment(); f.document->set_environment(e).value(); auto gpu=take(r.prepare_scene(q,f.scene()));
    CloudFieldCache cache; auto fields=take(cache.prepare(e.clouds)); PathMediumSettings ms; ms.time_seconds=23;
    auto m=take(PathMediumSnapshot::create(e,ms,fields)); auto description=f.view(f.source(),7).description();
    Trsd camera; camera.translation={700,2000,-500}; description.camera_world=take(Transformd::from_trs(camera)); auto view=take(RenderView::create(f.source(),description)); const auto initialView=view;
    auto s=air_settings(4); s.environment_rgb={.2,.4,.6}; s.sun=true;
    auto once=complete(q,take(r.submit_medium_view(q,gpu,m,view,s,{0,19})));
    const auto reference=take(render_volume_path_reference(heap,gpu.source(),m,view,s,{0,19}));
    for(std::size_t i=0;i<once.linear_rgb().size();++i) close_color(once.linear_rgb()[i],reference.linear_rgb()[i],2e-4);
    PathTraceSchedule schedule; schedule.target_spp=19;
    for(unsigned batch:{3,7,16}) {
        schedule.batch_spp=batch; schedule.tile_edge=batch==3 ? 4 : 2;
        check(r.configure_medium_view(q,1,gpu,m,view,s,schedule)); compare_images(finish_view(r,q,1),once,0); check(r.release_view(1));
    }
    check(r.configure_medium_view(q,1,gpu,m,view,s,schedule)); auto old=finish_view(r,q,1); auto held=r.completed_frame(1);
    const auto generation=take(r.status(1)).generation; schedule.target_spp=20; schedule.exposure=2;
    check(r.configure_medium_view(q,1,gpu,m,view,s,schedule)); require(take(r.status(1)).generation==generation,"cloud exposure reset history");
    const auto owned=r.memory_stats(q).owned.current; take(r.submit_batch(q,1));
    ms.time_seconds=24; auto next=take(PathMediumSnapshot::create(e,ms,fields)); check(r.configure_medium_view(q,1,gpu,next,view,s,schedule));
    require(!collect(r,q),"late old wind time published"); require(take(r.status(1)).completed_spp==0,"wind edit retained history");
    auto updated=finish_view(r,q,1); require(updated.info().medium->medium.time_seconds==24 && updated.info().medium->field_hash==fields->hash(),"wind provenance stale");
    require(r.memory_stats(q).owned.current>=owned,"external completed frame lost resource charge");
    compare_images(complete(q,take(held.read_linear(q))),old,0);
    const auto before=r.memory_stats(q).owned.current; ms.time_seconds=25;
    check(r.configure_medium_view(q,2,gpu,take(PathMediumSnapshot::create(e,ms,fields)),view,s,schedule));
    require(r.memory_stats(q).owned.current==before,"wind time duplicated cloud fields"); check(r.release_view(2));
    ++e.clouds.seed; f.document->set_environment(e).value(); auto newFields=take(cache.prepare(e.clouds)); auto changed=take(r.prepare_scene(q,f.scene()));
    auto newMedium=take(PathMediumSnapshot::create(e,ms,newFields)); view=take(RenderView::create(f.source(),description));
    check(r.configure_medium_view(q,2,changed,newMedium,view,s,schedule)); require(r.memory_stats(q).owned.current>before,"new cloud field was not charged");
    auto newImage=finish_view(r,q,2); require(newImage.info().medium->field_hash!=old.info().medium->field_hash,"new cloud field provenance unchanged");
    compare_images(complete(q,take(held.read_linear(q))),old,0);
    const auto both=r.memory_stats(q).owned.current; PathMediumSettings oldTime; oldTime.time_seconds=26;
    check(r.configure_medium_view(q,1,gpu,take(PathMediumSnapshot::create(m.environment(),oldTime,fields)),initialView,s,schedule));
    require(r.memory_stats(q).owned.current<=both,"wind edit duplicated a field still owned by another generation");
    check(r.release_view(1)); check(r.release_view(2));
    std::puts("cloud exact batches/tiles/wind reset/reuse/new field/late completion/old frame=pass");
}
void cloud_failure(memory::ResourceHandle heap,SubmissionQueue& q) {
    auto r=take(PathTraceRenderer::create(heap,q,DK_PATH_SHADER_DIR,{},1u<<20)); Fixture f{heap}; auto e=cloud_environment();
    f.document->set_environment(e).value(); auto gpu=take(r.prepare_scene(q,f.scene())); CloudFieldCache cache;
    auto fields=take(cache.prepare(e.clouds)); auto m=take(PathMediumSnapshot::create(e,{},fields)); auto view=f.view(f.source(),4);
    auto s=air_settings(3); s.environment_rgb=Vec3d::Ones(); PathTraceSchedule schedule; schedule.target_spp=2; schedule.batch_spp=2;
    check(r.configure_medium_view(q,1,gpu,m,view,s,schedule)); auto old=finish_view(r,q,1); auto held=r.completed_frame(1);
    const auto generation=take(r.status(1)).generation; schedule.target_spp=4; auto low=s; low.max_tracking_steps=1;
    check(r.configure_medium_view(q,1,gpu,m,view,low,schedule)); take(r.submit_batch(q,1)); q.device().logical_device().waitIdle();
    auto failed=r.poll(q); require(!failed && failed.error().message.find("budget")!=std::string::npos,"cloud tracking budget ignored");
    require(take(r.status(1)).completed_spp==2,"partial cloud candidate published");
    check(r.configure_medium_view(q,1,gpu,m,view,s,schedule)); fail_submit=true; require(!r.submit_batch(q,1),"cloud submit failure ignored");
    std::stop_source stop; take(r.submit_batch(q,1,stop.get_token())); stop.request_stop(); require(!collect(r,q),"cancelled cloud candidate published");
    require(finish_view(r,q,1).info().samples.count==4,"cloud recovery lost range");
    // Field preparation failure must leave current view, cached field, generation and old image intact.
    const auto before=r.memory_stats(q).owned.current,submitted=q.stats().submitted;
    auto huge=e; huge.clouds.base_resolution=128; f.document->set_environment(huge).value(); auto large=take(r.prepare_scene(q,f.scene()));
    auto largeFields=take(cache.prepare(huge.clouds)); auto tooLarge=take(PathMediumSnapshot::create(huge,{},largeFields));
    require(!r.configure_medium_view(q,1,large,tooLarge,f.view(f.source(),4),s,schedule),"cloud field exceeded renderer memory budget");
    require(r.memory_stats(q).owned.current==before && take(r.status(1)).generation==generation && take(r.status(1)).completed_spp==4,"failed cloud upload modified state");
    require(q.stats().submitted==submitted,"failed cloud upload submitted work");
    auto unsupported=e; unsupported.clouds.detail_scale_m=1; f.document->set_environment(unsupported).value(); auto bad=take(r.prepare_scene(q,f.scene()));
    require(!r.configure_medium_view(q,1,bad,take(PathMediumSnapshot::create(unsupported,{},fields)),f.view(f.source(),4),s,schedule),"unsupported cloud scale admitted");
    auto thin=e; thin.clouds.top_height_m=thin.clouds.bottom_height_m+1e-6; f.document->set_environment(thin).value();
    auto thinScene=take(r.prepare_scene(q,f.scene()));
    require(!r.configure_medium_view(q,1,thinScene,take(PathMediumSnapshot::create(thin,{},fields)),f.view(f.source(),4),s,schedule),"collapsed float cloud shell admitted");
    check(r.configure_medium_view(q,1,gpu,m,view,s,schedule)); compare_images(complete(q,take(held.read_linear(q))),old,0);
    require(r.memory_stats(q).owned.peak<=r.memory_stats(q).owned.limit,"cloud budget overshot"); check(r.release_view(1));
    std::printf("cloud tracking/submit/cancel/upload-budget/admission/recovery peak=%llu=pass\n",r.memory_stats(q).owned.peak);
}
}

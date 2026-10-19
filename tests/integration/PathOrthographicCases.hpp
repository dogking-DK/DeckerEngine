#pragma once
namespace {
void orthographic(memory::ResourceHandle heap,PathTraceRenderer& r,SubmissionQueue& q) {
    Fixture f{heap}; f.quad(-.5f,.5f,0,{0,0,0},{2,3,4});
    auto s=settings(1); auto source=f.source(); auto cpu=f.scene(); auto gpu=take(r.prepare_scene(q,cpu));
    auto d=f.view(f.source(),8).description(); d.projection=take(orthographic_projection({2,.25,2},1));
    auto view=take(RenderView::create(f.source(),d));
    auto base=complete(q,take(r.submit_view(q,gpu,view,s,{0,8})));
    auto reference=take(render_path_reference(heap,cpu,view,s,{0,8}));
    unsigned hits=0; for(std::size_t i=0;i<base.linear_rgb().size();++i) {
        close_color(base.linear_rgb()[i],reference.linear_rgb()[i],0);
        if(base.primary_hits()[i]) { ++hits; require(std::abs(base.primary_hits()[i]->distance-.75)<1e-6,"orthographic near origin distance"); }
    }
    require(hits==16,"orthographic coverage");
    for(double z:{-.5,.9,-1.5}) {
        Trsd t; t.translation.z()=z; check(f.document->set_local_transform(f.entity,t));
        auto changed=take(r.prepare_scene(q,f.scene())); auto v=take(RenderView::create(f.source(),d));
        auto image=complete(q,take(r.submit_view(q,changed,v,s,{0,8})));
        for(std::size_t i=0;i<base.linear_rgb().size();++i) close_color(image.linear_rgb()[i],z==-.5 ? base.linear_rgb()[i] : Vec3d::Zero(),0);
    }
    // Keep geometry away from the exact clip plane, whose float tie is not a pixel contract.
    auto perspective=d; Trsd pose; pose.translation={0,0,2}; perspective.camera_world=take(Transformd::from_trs(pose)); perspective.projection=take(perspective_projection({radians(60.),1,10},1));
    const auto pv=take(RenderView::create(source,perspective));
    auto pg=complete(q,take(r.submit_view(q,gpu,pv,s,{0,8}))); auto pc=take(render_path_reference(heap,cpu,pv,s,{0,8}));
    for(std::size_t i=0;i<pg.linear_rgb().size();++i) close_color(pg.linear_rgb()[i],pc.linear_rgb()[i],0);
    PathTraceSchedule schedule; schedule.target_spp=8; schedule.batch_spp=2; schedule.tile_edge=3;
    check(r.configure_view(q,701,gpu,view,s,schedule)); compare_images(finish_view(r,q,701),base,0);
    auto generation=take(r.status(701)).generation; auto submitted=q.stats().submitted;
    check(r.configure_view(q,701,gpu,view,s,schedule)); require(take(r.status(701)).generation==generation,"same orthographic view reset");
    auto invalid=d; invalid.projection.setIdentity(); const auto retained=r.completed_frame(701).linear_buffer();
    require(!r.configure_view(q,701,gpu,take(RenderView::create(source,invalid)),s,schedule),"invalid projection accepted");
    require(take(r.status(701)).generation==generation && r.completed_frame(701).linear_buffer()==retained,"rejected lens replaced completed frame");
    d.projection=take(orthographic_projection({3,.25,2},1));
    check(r.configure_view(q,701,gpu,take(RenderView::create(source,d)),s,schedule));
    require(take(r.status(701)).generation>generation && take(r.status(701)).completed_spp==0,"height failed to reset history");
    require(q.stats().submitted==submitted,"camera configuration submitted geometry/AS work");
    generation=take(r.status(701)).generation;
    check(r.configure_view(q,701,gpu,f.view(source,8),s,schedule));
    require(take(r.status(701)).generation>generation,"projection failed to reset history"); check(r.release_view(701));
    std::puts("orthographic surface CPU/GPU coverage depth near/far tiling history=pass");
}
void orthographic_medium(memory::ResourceHandle heap,PathTraceRenderer& r,SubmissionQueue& q) {
    for(bool clouds:{false,true}) {
        Fixture f{heap}; auto e=air_environment(.0001,0);
        if(clouds) { e.clouds=cloud_environment().clouds; e.clouds.extinction_per_m=.0004; }
        check(f.document->set_environment(e)); auto source=f.source(); auto cpu=f.scene(); auto gpu=take(r.prepare_scene(q,cpu));
        CloudFieldCache cache; std::shared_ptr<const CloudFieldData> field;
        if(clouds) field=take(cache.prepare(e.clouds));
        PathMediumSettings ms; ms.cloud_albedo=0; ms.time_seconds=17;
        auto medium=take(PathMediumSnapshot::create(e,ms,field)); auto s=air_settings(0); s.environment_rgb=Vec3d::Ones();
        auto d=f.view(f.source(),4).description(); Trsd camera; camera.translation={0,2000,0};
        camera.rotation=Quatd{Eigen::AngleAxisd(std::numbers::pi/4,Vec3d::UnitX())};
        d.camera_world=take(Transformd::from_trs(camera)); d.projection=take(orthographic_projection({4000,1000,1010},1));
        auto view=take(RenderView::create(f.source(),d));
        auto image=complete(q,take(r.submit_medium_view(q,gpu,medium,view,s,{0,512})));
        auto reference=take(render_volume_path_reference(heap,cpu,medium,view,s,{0,512}));
        double error=0,spread=0; for(std::size_t i=0;i<image.linear_rgb().size();++i) {
            error=std::max(error,(image.linear_rgb()[i]-reference.linear_rgb()[i]).cwiseAbs().maxCoeff());
            spread=std::max(spread,(reference.linear_rgb()[i]-reference.linear_rgb()[0]).norm());
        }
        std::printf("orthographic medium clouds=%d CPU/GPU max_abs=%g spread=%g\n",clouds,error,spread);
        require(error<.035 && spread>.1,"per-pixel atmosphere/cloud origin mismatch");
        // Preserve physical medium boundaries beyond mesh far; moving near changes the medium segment.
        d.projection=take(orthographic_projection({4000,1000,10000},1));
        auto far=complete(q,take(r.submit_medium_view(q,gpu,medium,take(RenderView::create(f.source(),d)),s,{0,512})));
        compare_images(far,image,0);
        PathTraceSchedule schedule; schedule.target_spp=7; schedule.batch_spp=2; schedule.tile_edge=3;
        check(r.configure_medium_view(q,702,gpu,medium,view,s,schedule));
        auto tiled=finish_view(r,q,702); auto once=complete(q,take(r.submit_medium_view(q,gpu,medium,view,s,{0,7})));
        compare_images(tiled,once,0); const auto generation=take(r.status(702)).generation;
        d.projection=take(orthographic_projection({2000,2000,10000},1));
        check(r.configure_medium_view(q,702,gpu,medium,take(RenderView::create(f.source(),d)),s,schedule));
        require(take(r.status(702)).generation>generation && take(r.status(702)).completed_spp==0,"medium lens history reset");
        check(r.release_view(702));
    }
}
}

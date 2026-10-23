#pragma once
namespace {
unsigned finish_grouped(PathTraceRenderer& r,SubmissionQueue& q,std::uint64_t id) {
    unsigned submissions=0;
    while(take(r.status(id)).completed_spp<take(r.status(id)).target_spp) {
        require(take(r.submit_batch(q,id)),"group did not submit"); ++submissions;
        require(!take(r.submit_batch(q,id)),"pending group submitted twice");
        require(take(q.wait(r.pending_submission())),"group wait failed"); (void)take(r.poll(q));
    }
    return submissions;
}
void tile_group_images(memory::ResourceHandle heap,PathTraceRenderer& r,SubmissionQueue& q) {
    Fixture f{heap}; auto e=air_environment(0,0); e.atmosphere.planet_center_m[1]-=100;
    check(f.document->set_environment(e)); f.quad(-8,8,0,{.6f,.4f,.2f}); f.quad(-1,1,2,{0,0,0},{3,2,1},true);
    const auto gpu=take(r.prepare_scene(q,f.scene())); const auto medium=take(PathMediumSnapshot::create(e));
    auto d=f.view(f.source(),33).description(); d.height=17; const auto view=take(RenderView::create(f.source(),d));
    auto s=settings(2); s.surface_only=true; s.environment_rgb={.3,.4,.6}; auto volume=air_settings(2); volume.environment_rgb=s.environment_rgb;
    for(bool media:{false,true}) for(unsigned mode=0;mode<5;++mode) {
        RestirDiSettings ris; if(mode==2 || mode==4) ris.temporal.emplace(); if(mode>=3) ris.spatial.emplace();
        PathTraceGpuImage reference;
        for(unsigned group:{1u,4u,8u}) {
            PathTraceSchedule schedule; schedule.target_spp=3; schedule.batch_spp=2; schedule.tile_edge=8;
            schedule.tiles_per_submission=group; schedule.reverse_tiles=group==8;
            auto configure=[&](const PathTraceSchedule& value) {
                if(media) check(mode ? r.configure_restir_medium_view(q,120,gpu,medium,view,volume,ris,value) :
                    r.configure_medium_view(q,120,gpu,medium,view,volume,value));
                else check(mode ? r.configure_restir_view(q,120,gpu,view,s,ris,value) : r.configure_view(q,120,gpu,view,s,value));
            };
            configure(schedule); const auto generation=take(r.status(120)).generation;
            for(unsigned invalid_group:{0u,9u}) {
            auto invalid=schedule; invalid.tiles_per_submission=invalid_group;
            if(media) require(!(mode ? r.configure_restir_medium_view(q,120,gpu,medium,view,volume,ris,invalid) :
                r.configure_medium_view(q,120,gpu,medium,view,volume,invalid)),"zero group accepted");
            else require(!(mode ? r.configure_restir_view(q,120,gpu,view,s,ris,invalid) :
                r.configure_view(q,120,gpu,view,s,invalid)),"zero group accepted");
            require(take(r.status(120)).generation==generation,"invalid group changed view");
            }
            const unsigned phases=mode ? 2u+unsigned(bool(ris.temporal))+unsigned(bool(ris.spatial)) : 1u;
            const unsigned expected=(mode ? 3u*phases : 2u)*((15u+group-1)/group);
            require(finish_grouped(r,q,120)==expected,"group did not reduce submission count as requested");
            auto image=complete(q,take(r.completed_frame(120).read_linear(q)));
            if(group==1) reference=image;
            else { compare_images(image,reference,0); compare_restir_statistics(image.info(),reference.info()); }
            check(r.release_view(120));
        }
    }
    // A requested group cannot multiply the caller's total ray-work budget.
    PathTraceSchedule schedule; schedule.target_spp=1; schedule.tile_edge=8; schedule.tiles_per_submission=8;
    s.max_rays=5u*64u; check(r.configure_view(q,120,gpu,view,s,schedule));
    require(finish_grouped(r,q,120)==15,"submission group exceeded ray-work budget"); check(r.release_view(120));
    std::puts("grouped surface/media classic/RIS/time/space images statistics tails reverse work bounds=pass");
}
void tile_group_state(memory::ResourceHandle heap,PathTraceRenderer& r,SubmissionQueue& q) {
    Fixture f{heap}; f.quad(-8,8,0,{.6f,.4f,.2f}); auto gpu=take(r.prepare_scene(q,f.scene()));
    auto s=settings(2); s.environment_rgb={.3,.4,.6}; auto d=f.view(f.source(),33).description(); d.height=17;
    const auto view=take(RenderView::create(f.source(),d)); PathTraceSchedule schedule;
    schedule.target_spp=1; schedule.tile_edge=8; schedule.tiles_per_submission=4;
    auto ris=spatial_settings(true); check(r.configure_restir_view(q,121,gpu,view,s,ris,schedule));
    finish_grouped(r,q,121); auto held=r.completed_frame(121); auto original=complete(q,take(held.read_linear(q)));
    schedule.target_spp=3; schedule.batch_spp=2; check(r.configure_restir_view(q,121,gpu,view,s,ris,schedule));
    require(take(r.submit_batch(q,121)),"first private group missing"); require(take(q.wait(r.pending_submission())),"private group wait");
    require(!take(r.poll(q)) && take(r.status(121)).completed_tiles==4,"partial group advanced full image");
    fail_submit=true; require(!r.submit_batch(q,121),"later group failure was ignored");
    require(r.completed_frame(121).linear_buffer()==held.linear_buffer() && take(r.status(121)).completed_spp==1,"failed group published");
    std::stop_source stop; require(take(r.submit_batch(q,121,stop.get_token())),"cancel group missing"); stop.request_stop();
    require(take(q.wait(r.pending_submission())),"cancel group drain"); require(!take(r.poll(q)),"cancelled group published");
    require(take(r.status(121)).completed_spp==1,"cancelled group advanced samples");
    finish_grouped(r,q,121); auto recovered=complete(q,take(r.completed_frame(121).read_linear(q)));
    compare_images(complete(q,take(held.read_linear(q))),original,0); check(r.release_view(121));
    check(r.configure_restir_view(q,122,gpu,view,s,ris,schedule)); finish_grouped(r,q,122);
    auto fresh=complete(q,take(r.completed_frame(122).read_linear(q))); compare_images(recovered,fresh,0);
    compare_restir_statistics(recovered.info(),fresh.info()); check(r.release_view(122));
    // Each 2x2 tile fits eight tracking steps. Sharing its counter across four
    // dispatches would incorrectly reject a valid group of sixteen primary paths.
    Fixture air{heap}; auto e=air_environment(1,0); e.atmosphere.planet_center_m[1]-=100;
    check(air.document->set_environment(e)); auto empty=take(r.prepare_scene(q,air.scene()));
    auto description=air.view(air.source(),9).description(); description.height=5;
    auto medium_view=take(RenderView::create(air.source(),description)); auto medium=take(PathMediumSnapshot::create(e));
    auto volume=air_settings(0); volume.environment_rgb=Vec3d::Ones(); volume.max_tracking_steps=8; volume.max_steps_per_path=4;
    PathTraceGpuImage ungrouped;
    for(unsigned group:{1u,4u}) {
        schedule={}; schedule.target_spp=1; schedule.tile_edge=2; schedule.tiles_per_submission=group;
        check(r.configure_medium_view(q,123,empty,medium,medium_view,volume,schedule)); finish_grouped(r,q,123);
        auto image=complete(q,take(r.completed_frame(123).read_linear(q)));
        if(group==1) ungrouped=image; else compare_images(image,ungrouped,0);
        check(r.release_view(123));
    }
    // Later-slice failure must reject the whole group and retain completed HDR.
    schedule.tiles_per_submission=4; check(r.configure_medium_view(q,123,empty,medium,medium_view,volume,schedule));
    finish_grouped(r,q,123); auto good=r.completed_frame(123);
    auto low=volume; low.max_tracking_steps=1; schedule.target_spp=2;
    check(r.configure_medium_view(q,123,empty,medium,medium_view,low,schedule));
    require(take(r.submit_batch(q,123)),"medium failing group missing"); require(take(q.wait(r.pending_submission())),"medium failed group drain");
    require(!r.poll(q) && r.completed_frame(123).linear_buffer()==good.linear_buffer() && take(r.status(123)).completed_spp==1,
        "slice tracking error partially published"); check(r.release_view(123));
    // Retained owners leave enough for one tile's diagnostics while an eight-tile
    // diagnostic group must fail. Releasing no owner, switching to one recovers.
    auto bounded=take(PathTraceRenderer::create(heap,q,DK_PATH_SHADER_DIR,{},1ull<<20));
    auto small=take(bounded.prepare_scene(q,f.scene())); auto small_view=f.view(f.source(),20);
    std::vector<PathTraceCompletedFrame> owners; schedule={}; schedule.tile_edge=2; schedule.tiles_per_submission=8;
    bool rejected=false;
    for(unsigned sample=1;sample<24;++sample) {
        schedule.target_spp=sample; check(bounded.configure_restir_view(q,124,small,small_view,s,ris,schedule));
        auto submitted=bounded.submit_batch(q,124);
        if(!submitted) {
            const auto before=bounded.completed_frame(124).linear_buffer();
            schedule.tiles_per_submission=1; check(bounded.configure_restir_view(q,124,small,small_view,s,ris,schedule));
            require(bounded.completed_frame(124).linear_buffer()==before,"budget failure changed completed owner");
            finish_grouped(bounded,q,124); rejected=true; break;
        }
        require(*submitted,"bounded group missing"); require(take(q.wait(bounded.pending_submission())),"bounded first group wait");
        (void)take(bounded.poll(q)); finish_grouped(bounded,q,124); owners.push_back(bounded.completed_frame(124));
    }
    require(rejected,"grouped diagnostics were not charged to allocation budget"); check(bounded.release_view(124));
    std::puts("grouped later-submit failure cancellation recovery full publication and immutable owners=pass");
}
}

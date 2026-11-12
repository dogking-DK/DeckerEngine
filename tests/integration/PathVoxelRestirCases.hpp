#pragma once
namespace {
RestirDiSettings voxel_ris(unsigned mode) {
    RestirDiSettings r;
    if(mode&1) {r.temporal.emplace(); r.temporal->position_tolerance=1; r.temporal->max_history_age=4;}
    if(mode&2) {r.spatial.emplace(); r.spatial->position_tolerance=2; r.spatial->max_source_candidates=8;}
    return r;
}
RenderView voxel_signal_view(const path_voxel_test::Fixture& f,bool ortho=false,double x=.5,unsigned size=5) {
    auto d=f.view(size).description(); Trsd camera; camera.translation={x,.5,2}; d.camera_world=take(Transformd::from_trs(camera));
    d.projection(0,0)=1.25; d.projection(1,1)=-1.25;
    if(ortho) d.projection=take(orthographic_projection({1.5,.1,10},1));
    return take(RenderView::create(f.source(),d));
}
void voxel_restir_numeric(memory::ResourceHandle heap,PathTraceRenderer& r,SubmissionQueue& q) {
    path_voxel_test::Fixture f{heap}; const std::array cells{assets::VoxelBlock{{-1,0,0},1,1},assets::VoxelBlock{{0,0,0},1,2}}; f.grid(cells);
    f.quad(-2,2,2,{.3f,.3f,.3f},{2,3,4});
    for(const auto model:{PathTraceMaterialModel::lambert,PathTraceMaterialModel::metallic_roughness}) {
        if(model==PathTraceMaterialModel::metallic_roughness) {f.voxel.voxel->palette[0].metallic=.6f; f.voxel.voxel->palette[0].roughness=.35f;}
        const auto cpu=f.scene(model),oracle=f.oracle(model); const auto gpu=take(r.prepare_scene(q,cpu));
        auto s=settings(3); s.environment_rgb={.2,.3,.4}; s.rr_start=2;
        for(const PathTraceRay ray:{PathTraceRay{{-.3,.6,1.5},{0,0,-1}},PathTraceRay{{.3,.6,1.5},{0,0,-1}},PathTraceRay{{.3,.6,1.5},{0,0,1}}}) {
            const auto expected=take(estimate_restir_reference(oracle,ray,s,{}, {0,2048}));
            const auto native=take(estimate_restir_reference(cpu,ray,s,{}, {0,2048}));
            const auto actual=complete(q,take(r.submit_restir_rays(q,gpu,std::span{&ray,1},s,{}, {0,2048})));
            close_color(native.mean,expected.mean,2e-4); close_color(actual.linear_rgb()[0],expected.mean,.006);
            close_color(actual.second_moment()[0],expected.second_moment,.025);
            require(actual.primary_hits()[0]->kind==take(cpu.nearest(ray))->kind,"RIS diagnostic lost geometry type");
        }
    }
    // Delta sunlight remains a separate visibility query: a thin voxel shadows a mesh,
    // and a mesh shadows a voxel. Both paths use the current procedural TLAS.
    for(bool voxel_receiver:{false,true}) {
        path_voxel_test::Fixture shadow{heap}; const std::array block{assets::VoxelBlock{{0,0,0},1,1}};
        Trsd thin; thin.scale={1,1,.001}; shadow.grid(block,take(Transformd::from_trs(thin)));
        shadow.quad(-2,2,voxel_receiver ? .5f : -.5f,{.7f,.6f,.5f});
        EnvironmentDescription env; env.sun.direction={0,0,1}; env.sun.irradiance={2,3,4}; check(shadow.document->set_environment(env));
        auto s=settings(); s.sun=PathTraceSun::scene;
        const PathTraceRay ray{{.3,.6,voxel_receiver ? .2 : -.2},{0,0,-1}};
        const auto gpu=take(r.prepare_scene(q,shadow.scene()));
        close_color(complete(q,take(r.submit_restir_rays(q,gpu,std::span{&ray,1},s,{}, {0,8}))).linear_rgb()[0],Vec3d::Zero(),1e-7);
    }
    std::puts("voxel RIS CPU/native/independent triangles GGX/indirect/RR/area/mixed thin shadows=pass");
}
void voxel_restir_reuse(memory::ResourceHandle heap,PathTraceRenderer& r,SubmissionQueue& q) {
    path_voxel_test::Fixture f{heap}; std::vector<assets::VoxelBlock> blocks;
    for(int y=-2;y<3;++y) for(int x=-2;x<3;++x) blocks.push_back({{x,y,0},1,1}); f.grid(blocks);
    const auto gpu=take(r.prepare_scene(q,f.scene())),tri=take(r.prepare_scene(q,f.oracle()));
    auto s=settings(); s.environment_rgb={.7,.8,.9}; const auto view=voxel_signal_view(f,false,.5,4);
    // Each independent seeded chain is one observation; compare its complete image mean
    // with the analytic Lambert/environment value (no false per-pixel independence).
    const Vec3d truth=Vec3f{.6f,.4f,.2f}.cast<double>().cwiseProduct(s.environment_rgb);
    for(unsigned mode=0;mode<4;++mode) {
        const auto ris=voxel_ris(mode); Vec3d mean=Vec3d::Zero();
        for(unsigned chain=0;chain<4;++chain) {
            s.seed=101+7919*chain; PathTraceSchedule schedule; schedule.target_spp=128; schedule.batch_spp=16;
            check(r.configure_restir_view(q,1,gpu,view,s,ris,schedule)); const auto image=finish_view(r,q,1);
            for(const auto& c:image.linear_rgb()) mean+=c/(4*16.);
            if(mode&1) require(image.info().temporal_statistics.reused>0 && image.info().temporal_statistics.max_age<=4,"voxel temporal inactive/unbounded");
            if(mode&2) require(image.info().spatial_statistics.reused>0 && image.info().spatial_statistics.current_visibility_rays>0,"voxel spatial/visibility inactive");
            if(mode==0 || mode==2) {
                // Initial and spatial paths have no primitive-dependent temporal acceptance.
                auto td=view.description(); const auto tv=take(RenderView::create(dk::render::detail::PathReferenceAccess::get(tri.source())->source,td));
                check(r.configure_restir_view(q,2,tri,tv,s,ris,schedule)); const auto reference=finish_view(r,q,2);
                for(unsigned i=0;i<16;++i) {close_color(image.linear_rgb()[i],reference.linear_rgb()[i],3e-4); close_color(image.second_moment()[i],reference.second_moment()[i],3e-4);}
                check(r.release_view(2));
            }
            check(r.release_view(1));
        }
        std::printf("voxel DI mode=%u independent-chain mean %.6f truth %.6f\n",mode,mean.x(),truth.x()); close_color(mean,truth,.025);
    }
    // Differently oriented faces have distinct light support. Spatial reuse intentionally permits
    // cross-face sources, re-evaluating each source's support rather than sharing a BSDF.
    path_voxel_test::Fixture edge{heap}; const std::array cube{assets::VoxelBlock{{0,0,0},1,1},assets::VoxelBlock{{0,1,0},1,2}}; edge.grid(cube);
    edge.voxel.voxel->palette[0].metallic=.6f; edge.voxel.voxel->palette[0].roughness=.35f;
    auto d=edge.view(5).description(); Trsd camera; camera.translation={2,.5,2}; camera.rotation=Eigen::AngleAxisd(std::numbers::pi/4,Vec3d::UnitY()); d.camera_world=take(Transformd::from_trs(camera));
    const auto edge_view=take(RenderView::create(edge.source(),d)); const auto ecpu=edge.scene(PathTraceMaterialModel::metallic_roughness),oracle=edge.oracle(PathTraceMaterialModel::metallic_roughness);
    const auto egpu=take(r.prepare_scene(q,ecpu)); auto ris=voxel_ris(2); ris.spatial->normal_cosine=0; ris.spatial->position_tolerance=4;
    PathTraceSchedule schedule; schedule.target_spp=64; schedule.batch_spp=16; s.seed=1597;
    check(r.configure_restir_view(q,1,egpu,edge_view,s,ris,schedule)); const auto image=finish_view(r,q,1);
    const auto ov=take(RenderView::create(dk::render::detail::PathReferenceAccess::get(oracle)->source,d));
    const auto expected=restir_spatial_oracle::render(oracle,ov,s,ris,64);
    for(unsigned i=0;i<25;++i) close_color(image.linear_rgb()[i],expected[i],3e-4);
    require(image.info().spatial_statistics.support_corrected>0,"voxel edge fixture missed source support correction"); check(r.release_view(1));
    std::puts("voxel four DI modes analytic chains/independent triangles/cross-face source support=pass");
}
void voxel_signals(memory::ResourceHandle heap,PathTraceRenderer& r,SubmissionQueue& q) {
    for(bool ortho:{false,true}) for(unsigned mode=0;mode<5;++mode) {
        path_voxel_test::Fixture f{heap}; const std::array cells{assets::VoxelBlock{{-1,0,0},1,1},assets::VoxelBlock{{0,0,0},1,2}}; f.grid(cells);
        f.quad(-3,3,-.2f,{.3f,.4f,.5f},{.01f,.02f,.03f});
        f.voxel.voxel->palette[0].metallic=.5f; f.voxel.voxel->palette[0].roughness=.3f;
        const auto cpu=f.scene(PathTraceMaterialModel::metallic_roughness); const auto gpu=take(r.prepare_scene(q,cpu));
        const auto view=voxel_signal_view(f,ortho,0); auto s=settings(3); s.environment_rgb={.7,.8,.9};
        PathTraceSchedule schedule; schedule.target_spp=7; schedule.batch_spp=4; schedule.tile_edge=2; schedule.tiles_per_submission=3;
        auto configure=[&](unsigned id,const RenderView& v) {if(mode) check(r.configure_restir_view(q,id,gpu,v,s,voxel_ris(mode-1),schedule)); else check(r.configure_view(q,id,gpu,v,s,schedule));};
        configure(1,view); const auto off=finish_view(r,q,1);
        schedule.signals.enabled=true; schedule.reverse_tiles=true; configure(2,view); const auto on=finish_view(r,q,2);
        compare_images(off,on,0); signal_recompose(on); require(on.signal_identities().size()==25,"missing resolved surface identities");
        require(on.info().signals->source.content_version==cpu.content_version(),"signal content owner mismatch");
        unsigned mesh=0,voxel=0,negative=0;
        for(unsigned i=0;i<25;++i) {
            const auto& p=on.denoising_signals()[i]; const auto& id=on.signal_identities()[i];
            const auto ray=take(path_trace_camera_ray(view,i%5,i/5,{path_trace_sample(s.seed,i,p.identity[1],0,0),path_trace_sample(s.seed,i,p.identity[1],0,1)}));
            const auto hit=take(cpu.nearest(ray)); require(bool(id)==bool(hit),"representative surface identity mismatch");
            if(!hit) continue;
            require(id->kind==hit->kind && id->entity==hit->entity && id->asset==hit->mesh && id->content_version==cpu.content_version(),"resolved frozen identity mismatch");
            close_color(signal_rgb(p.position_view_depth),hit->position,1e-5); close_color(signal_rgb(p.normal_roughness),hit->normal,1e-6);
            require(std::abs(p.motion_distance[3]-hit->distance)<1e-5,"voxel signal ray distance mismatch");
            if(id->kind==PathGeometryKind::voxel) {
                const auto& material=f.voxel.voxel->palette[id->value-1]; close_color(signal_rgb(p.albedo_metallic),material.base_color.cast<double>(),1e-6);
                require(std::abs(p.normal_roughness[3]-material.roughness)<1e-6 && std::abs(p.albedo_metallic[3]-material.metallic)<1e-6,"voxel roughness/metallic guide mismatch");
                ++voxel; negative+=id->cell[0]<0; require(id->cell==hit->cell && id->face==hit->face && id->value==hit->value && p.identity[0]==0xffffffffu,"voxel cell/face/palette confused with triangle");}
            else {++mesh; require(id->primitive==hit->primitive && id->triangle==hit->triangle,"mesh identity changed");}
        }
        require(mesh && voxel && negative,"mixed negative-cell guide fixture lacks coverage");
        const auto old=r.completed_frame(2); schedule.signals.previous=old.signal_reference(); schedule.target_spp=schedule.batch_spp=1;
        const auto moved=voxel_signal_view(f,ortho,.04); configure(2,moved); const auto motion=finish_view(r,q,2); signal_recompose(motion);
        unsigned valid=0;
        for(unsigned i=0;i<25;++i) {const auto& p=motion.denoising_signals()[i]; if(!(p.identity[2]&2)) continue; ++valid;
            const Vec4d clip=schedule.signals.previous->world_to_clip*Vec4d{p.position_view_depth[0],p.position_view_depth[1],p.position_view_depth[2],1};
            const Vec2d expected{(clip.x()/clip.w()+1)*2.5-(i%5+path_trace_sample(s.seed,i,p.identity[1],0,0)),(clip.y()/clip.w()+1)*2.5-(i/5+path_trace_sample(s.seed,i,p.identity[1],0,1))};
            require((Vec2d{p.motion_distance[0],p.motion_distance[1]}-expected).norm()<3e-5,"voxel motion units/jitter mismatch");
        }
        require(valid>0,"voxel motion invalid"); const auto retained=complete(q,take(old.read_linear(q)));
        require(std::memcmp(retained.denoising_signals().data(),on.denoising_signals().data(),on.denoising_signals().size_bytes())==0,"old voxel signal buffer changed");
        check(r.release_view(1)); check(r.release_view(2));
    }
    std::puts("voxel Classic/four DI signals raw-exact/recompose/negative-cell/mixed identities/perspective/orthographic/motion=pass");
}
void voxel_restir_state(memory::ResourceHandle heap,PathTraceRenderer& r,SubmissionQueue& q) {
    path_voxel_test::Fixture f{heap}; const std::array cube{assets::VoxelBlock{{0,0,0},1,1}}; f.grid(cube);
    auto gpu=take(r.prepare_scene(q,f.scene())); auto view=voxel_signal_view(f); auto s=settings(); s.environment_rgb=Vec3d::Ones(); auto ris=voxel_ris(3);
    PathTraceSchedule schedule; schedule.signals.enabled=true; schedule.target_spp=schedule.batch_spp=2;
    check(r.configure_restir_view(q,1,gpu,view,s,ris,schedule)); const auto original=finish_view(r,q,1); const auto old=r.completed_frame(1);
    const auto serial=old.info().signals->source.source_serial; schedule.target_spp=4;
    // One tile per phase. Inject failures at prepare/temporal/spatial/shade separately.
    for(unsigned phase=0;phase<4;++phase) {
        check(r.configure_restir_view(q,1,gpu,view,s,ris,schedule));
        for(unsigned i=0;i<phase;++i) {take(r.submit_batch(q,1)); collect(r,q);}
        fail_submit=true; require(!r.submit_batch(q,1) && !fail_submit,"RIS phase failure injection missed submit"); collect(r,q);
        require(r.completed_frame(1).info().signals->source.source_serial==serial && take(r.status(1)).completed_spp==2,"partial RIS epoch published");
        check(r.cancel(1)); collect(r,q);
    }
    std::stop_source stopped; take(r.submit_batch(q,1,stopped.get_token())); stopped.request_stop(); collect(r,q);
    require(r.completed_frame(1).info().signals->source.source_serial==serial,"cancelled RIS epoch published");
    signal_recompose(finish_view(r,q,1));
    schedule.target_spp=schedule.batch_spp=1;
    for(const double x:{5.,.5}) {
        schedule.signals.previous=r.completed_frame(1).signal_reference();
        const auto moved=voxel_signal_view(f,false,x); check(r.configure_restir_view(q,1,gpu,moved,s,ris,schedule)); const auto reveal=finish_view(r,q,1);
        require(reveal.info().temporal_statistics.reused==0 && reveal.info().temporal_statistics.rejected==25,"camera reveal reused unrelated cell/miss history");
        signal_recompose(reveal);
        if(x==5.) for(const auto& id:reveal.signal_identities()) require(!id,"camera miss retained voxel identity");
    }
    const auto reference=*r.completed_frame(1).signal_reference(); schedule.signals.previous=reference;
    schedule.signals.previous->source.version=1;
    require(!r.configure_restir_view(q,1,gpu,view,s,ris,schedule),"v1 signal reference accepted");
    DenoisingMotionReference forged; forged.source=reference.source; forged.world_to_clip=reference.world_to_clip;
    schedule.signals.previous=forged; require(!r.configure_restir_view(q,1,gpu,view,s,ris,schedule),"hash-only forged reference accepted");
    for(unsigned change=0;change<3;++change) {
        const auto previous=*r.completed_frame(1).signal_reference(); const auto generation=take(r.status(1)).generation;
        if(change==0) f.voxel.voxel->palette[0].base_color={.1f,.7f,.3f};
        if(change==1) {const std::array moved{assets::VoxelBlock{{1,0,0},1,1}}; f.grid(moved);}
        if(change==2) {Trsd transform; transform.translation={-.5,0,0}; f.transform(transform);}
        const auto candidate=take(r.prepare_scene(q,f.scene())); schedule.signals.previous=previous;
        // Even forged public metadata cannot turn an old CPU owner into the new source.
        schedule.signals.previous->source.fingerprint=candidate.source().fingerprint(); schedule.signals.previous->source.geometry_fingerprint=candidate.source().geometry_fingerprint(); schedule.signals.previous->source.content_version=candidate.source().content_version();
        require(!r.configure_restir_view(q,1,candidate,view,s,ris,schedule),"old content owner accepted");
        require(take(r.status(1)).generation==generation,"rejected reference changed generation");
        schedule.signals.previous.reset(); gpu=candidate; view=voxel_signal_view(f); schedule.target_spp=2;
        check(r.configure_restir_view(q,1,gpu,view,s,ris,schedule)); const auto changed=finish_view(r,q,1);
        require(changed.info().sampling_first_epoch==0 && !changed.info().temporal_history_epoch,"content edit retained RIS history");
        check(r.configure_restir_view(q,2,gpu,view,s,ris,schedule)); compare_images(changed,finish_view(r,q,2),0); check(r.release_view(2));
    }
    schedule.target_spp=4; check(r.configure_restir_view(q,1,gpu,view,s,ris,schedule)); take(r.submit_batch(q,1)); check(r.release_view(1)); collect(r,q);
    const auto retained=complete(q,take(old.read_linear(q))); compare_images(original,retained,0);
    require(std::memcmp(original.denoising_signals().data(),retained.denoising_signals().data(),original.denoising_signals().size_bytes())==0,"old complete signal owner corrupted");
    {auto bounded=take(PathTraceRenderer::create(heap,q,DK_PATH_SHADER_DIR,{},2ull<<20)); const auto small=take(bounded.prepare_scene(q,f.scene()));
        schedule.target_spp=2; check(bounded.configure_restir_view(q,1,small,view,s,ris,schedule)); const auto before=finish_view(bounded,q,1);
        require(!bounded.configure_restir_view(q,1,small,voxel_signal_view(f,false,.5,128),s,ris,schedule),"v2 RIS/signal memory budget ignored");
        compare_images(before,complete(q,take(bounded.completed_frame(1).read_linear(q))),0); check(bounded.release_view(1));}
    std::puts("voxel RIS all phase failures/cancel/release/v1 and forged owner rejection/content reset/immutable epoch/budget=pass");
}
}

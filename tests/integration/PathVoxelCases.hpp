#pragma once
#include "../PathVoxelSupport.hpp"
namespace {
void voxel_numeric(memory::ResourceHandle heap,PathTraceRenderer& renderer,SubmissionQueue& queue) {
    path_voxel_test::Fixture f{heap};
    const std::array blocks{assets::VoxelBlock{{-8,0,0},8,1,false},assets::VoxelBlock{{0,0,0},1,2}}; f.grid(blocks);
    for(auto& m:f.voxel.voxel->palette) m.base_color.setZero(); // Isolate primary traversal from BSDF rays.
    for(const double scale:{.00001,1.0,1000.0}) {
        Trsd trs; trs.scale={-scale,2*scale,.5*scale}; trs.translation={2*scale,-scale,3*scale};
        trs.rotation=Eigen::AngleAxisd(.4,Vec3d{1,2,3}.normalized()); f.transform(trs);
        const auto cpu=f.scene(),oracle=f.oracle(); auto gpu=take(renderer.prepare_scene(queue,cpu));
        std::vector<PathTraceRay> rays;
        for(const PathTraceRay local:std::array{PathTraceRay{{-4,.3,.7},{1,0,0}},PathTraceRay{{2,.3,.7},{-1,0,0}},
            PathTraceRay{{-4.37,2.29,10},{0,0,-1}},PathTraceRay{{2.23,2.37,2.51},{1,0,0}},PathTraceRay{{-4,.3,.7},{-1,0,0}}})
            rays.push_back({f.world.transform_point(local.origin).value(),f.world.transform_direction(local.direction).value().normalized()});
        auto image=complete(queue,take(renderer.submit_rays(queue,gpu,rays,settings(),{0,1})));
        for(std::size_t i=0;i<rays.size();++i) {
            auto a=take(cpu.nearest(rays[i])),b=take(oracle.nearest(rays[i])); const auto& hit=image.primary_hits()[i];
            require(bool(hit)==bool(a) && bool(a)==bool(b),"voxel primary presence differs from independent triangles");
            if(hit) {
                std::printf("scale %g ray %zu t %.9g expected %.9g cell %d %d %d face %u\n",scale,i,hit->distance,b->distance,hit->cell[0],hit->cell[1],hit->cell[2],hit->face);
                require(hit->kind==PathGeometryKind::voxel && hit->cell==a->cell && hit->face==a->face && hit->value==a->value,"typed voxel primary mismatch");
                require(std::abs(hit->distance-b->distance)<scale*3e-5,"voxel distance differs from triangles");
                require(hit->normal.dot(b->normal)>.9999,"voxel inverse transpose normal mismatch");
                require((hit->position-(rays[i].origin+hit->distance*rays[i].direction)).norm()<scale*3e-5,"voxel diagnostic position mismatch");
            }
        }
    }
    f.transform({});
    const std::array joined{assets::VoxelBlock{{-8,0,0},8,1},assets::VoxelBlock{{0,0,0},8,2}}; f.grid(joined);
    auto parallel=take(renderer.prepare_scene(queue,f.scene()));
    for(const PathTraceRay ray:std::array{PathTraceRay{{0,-1,.3},{0,1,0}},PathTraceRay{{0,.3,.3},{0,1,0}}}) {
        const auto expected=take(f.scene().nearest(ray)); auto image=ray_image(renderer,queue,parallel,settings(),1,ray);
        require(image.primary_hits()[0] && expected && image.primary_hits()[0]->cell==expected->cell && image.primary_hits()[0]->face==expected->face,"parallel ray lost on an internal block plane");
    }
    for(const unsigned dimension:{128u,4096u}) {
        const std::array tiles{assets::VoxelBlock{{-int(dimension),0,0},dimension,1},assets::VoxelBlock{{0,0,0},1,2}}; f.grid(tiles);
        auto gpu=take(renderer.prepare_scene(queue,f.scene())); const PathTraceRay ray{{-double(dimension)/2,.3,.7},{1,0,0}};
        auto image=ray_image(renderer,queue,gpu,settings(),1,ray); require(bool(image.primary_hits()[0]),"constant tile exit missing");
        require(image.primary_hits()[0]->cell==geometry::voxels::Coord{0,0,0},"tile/leaf seam became a surface");
        require(std::abs(image.primary_hits()[0]->distance-(dimension/2.0+1))<1e-3,"constant tile distance mismatch");
    }
    const std::array cube{assets::VoxelBlock{{0,0,0},1,1}}; f.grid(cube);
    auto boundary=take(renderer.prepare_scene(queue,f.scene()));
    for(const PathTraceRay ray:std::array{PathTraceRay{{-1,-1,-1},Vec3d::Ones().normalized()},PathTraceRay{{0,.3,.7},{1,0,0}},
        PathTraceRay{{-.5,.3,.7},{1,0,0},.5,2},PathTraceRay{{-.5,.3,.7},{1,0,0},.6,2}}) {
        const auto expected=take(f.scene().nearest(ray)); auto image=ray_image(renderer,queue,boundary,settings(),1,ray);
        require(image.primary_hits()[0] && expected && image.primary_hits()[0]->face==expected->face && image.primary_hits()[0]->cell==expected->cell,"analytic corner or clipped-start mismatch");
        require(std::abs(image.primary_hits()[0]->distance-expected->distance)<1e-5,"analytic boundary distance mismatch");
    }
    f.quad(-4,4,1.5f,{.8f,.8f,.8f});
    auto mixed=take(renderer.prepare_scene(queue,f.scene()));
    for(const PathTraceRay ray:std::array{PathTraceRay{{.3,.7,2},{0,0,-1}},PathTraceRay{{.3,.7,1.2},{0,0,-1}},PathTraceRay{{.3,.7,.5},{0,0,1}}}) {
        const auto expected=take(f.scene().nearest(ray)); auto image=ray_image(renderer,queue,mixed,settings(),1,ray);
        require(bool(image.primary_hits()[0]) && image.primary_hits()[0]->kind==expected->kind,"mixed nearest geometry mismatch");
        require(std::abs(image.primary_hits()[0]->distance-expected->distance)<1e-5,"mixed nearest distance mismatch");
    }
    auto clipped=ray_image(renderer,queue,mixed,settings(),1,{{.3,.7,1.2},{0,0,-1},0,.1}); require(!clipped.primary_hits()[0],"voxel ignored ray maximum");
    f.grid({}); auto empty=take(renderer.prepare_scene(queue,f.scene()));
    require(!ray_image(renderer,queue,empty,settings(),1,{{.3,.7,1.2},{0,0,-1}}).primary_hits()[0],"empty voxel generated a hit");
    path_voxel_test::Fixture multi{heap},second{heap}; Mat4d shear=Mat4d::Identity(); shear(0,1)=.3; shear(1,2)=.2;
    multi.grid(cube,Transformd::from_matrix(shear).value());
    const std::array negative{assets::VoxelBlock{{-1,-1,-1},1,2}}; second.grid(negative);
    auto duplicate=multi.document->create_entity().value(); multi.document->set_asset_references(duplicate,{{multi.voxel.voxel->id,AssetKind::voxel}}).value();
    Trsd left; left.translation={-3,0,0}; multi.document->set_local_transform(duplicate,left).value();
    auto other=multi.document->create_entity().value(); multi.document->set_asset_references(other,{{second.voxel.voxel->id,AssetKind::voxel}}).value();
    Trsd right; right.translation={5,0,0}; multi.document->set_local_transform(other,right).value();
    const std::array<const CpuAsset*,3> assets{&multi.voxel,&multi.mesh,&second.voxel};
    auto cpu=take(PathTraceScene::create(heap,multi.source(),assets)); auto multiple=take(renderer.prepare_scene(queue,cpu));
    for(const PathTraceRay ray:std::array{PathTraceRay{{.65,.7,2},{0,0,-1}},PathTraceRay{{-2.35,.7,2},{0,0,-1}},PathTraceRay{{4.5,-.5,1},{0,0,-1}}}) {
        auto expected=take(cpu.nearest(ray)); auto image=ray_image(renderer,queue,multiple,settings(),1,ray); const auto& hit=image.primary_hits()[0];
        require(hit && expected && hit->entity==expected->entity && hit->cell==expected->cell && hit->face==expected->face && hit->value==expected->value,"packed/shared Nano grid instance mismatch");
        require(std::abs(hit->distance-expected->distance)<1e-5 && hit->normal.dot(expected->normal)>.9999,"shear or multi-instance hit mismatch");
    }
}
void voxel_lighting(memory::ResourceHandle heap,PathTraceRenderer& r,SubmissionQueue& q) {
    path_voxel_test::Fixture f{heap}; const std::array cube{assets::VoxelBlock{{0,0,0},1,1}}; f.grid(cube);
    const PathTraceRay down_voxel{{.37,.63,1.25},{0,0,-1}},up_mesh{{.37,.63,1.25},{0,0,1}};
    auto s=settings(2); s.estimator=PathTraceEstimator::bsdf_only; s.environment_rgb={.7,.8,.9};
    for(const double scale:{.00001,1.0,1000.0}) {
        Trsd trs; trs.scale={-scale,2*scale,.5*scale}; trs.translation={2*scale,-scale,3*scale};
        trs.rotation=Eigen::AngleAxisd(.4,Vec3d{1,2,3}.normalized()); f.transform(trs);
        PathTraceRay ray{f.world.transform_point(down_voxel.origin).value(),f.world.transform_direction(down_voxel.direction).value().normalized()};
        auto gpu=take(r.prepare_scene(q,f.scene()));
        close_color(ray_image(r,q,gpu,s,256,ray).linear_rgb()[0],Vec3d{.6f,.4f,.2f}.cwiseProduct(s.environment_rgb),2e-4);
    }
    f.transform({}); EnvironmentDescription env; env.sun.direction={0,0,1}; env.sun.irradiance={2,3,4}; f.document->set_environment(env).value();
    s=settings(1); s.sun=PathTraceSun::scene;
    close_color(ray_image(r,q,take(r.prepare_scene(q,f.scene())),s,16,down_voxel).linear_rgb()[0],Vec3d{.6f,.4f,.2f}.cwiseProduct(Vec3d{2,3,4})/std::numbers::pi);
    f.quad(-.5f,1.5f,1.5f,{.7f,.6f,.5f}); auto mixed=take(r.prepare_scene(q,f.scene()));
    close_color(ray_image(r,q,mixed,s,16,down_voxel).linear_rgb()[0],Vec3d::Zero()); // mesh shadows voxel
    path_voxel_test::Fixture reverse{heap}; reverse.grid(cube); reverse.quad(-2,2,-1,{.7f,.6f,.5f}); reverse.document->set_environment(env).value();
    close_color(ray_image(r,q,take(r.prepare_scene(q,reverse.scene())),s,16,{{.37,.63,-.5},{0,0,-1}}).linear_rgb()[0],Vec3d::Zero()); // voxel shadows mesh
    env.sun.irradiance={0,0,0}; f.document->set_environment(env).value();
    for(const auto model:{PathTraceMaterialModel::lambert,PathTraceMaterialModel::metallic_roughness}) {
        if(model==PathTraceMaterialModel::metallic_roughness) {f.voxel.voxel->palette[0].metallic=.6f; f.voxel.voxel->palette[0].roughness=.35f;}
        const auto cpu=f.scene(model),oracle=f.oracle(model); auto gpu=take(r.prepare_scene(q,cpu));
        s=settings(4); s.environment_rgb={.7,.8,.9}; s.estimator=PathTraceEstimator::mis;
        for(const auto ray:{down_voxel,up_mesh}) {
            const auto expected=take(estimate_path_reference(oracle,ray,s,{0,4096}));
            const auto native=take(estimate_path_reference(cpu,ray,s,{0,4096}));
            auto image=ray_image(r,q,gpu,s,4096,ray);
            std::printf("mixed model %u first %s gpu %.6g cpu %.6g triangles %.6g\n",unsigned(model),ray.direction.z()<0 ? "voxel" : "mesh",image.linear_rgb()[0].x(),native.mean.x(),expected.mean.x());
            close_color(native.mean,expected.mean,2e-4); close_color(image.linear_rgb()[0],expected.mean,.006);
            close_color(image.second_moment()[0],expected.second_moment,.008);
            auto one=s; one.max_bounces=1; auto direct=take(estimate_path_reference(oracle,ray,one,{0,4096}));
            require((expected.mean-direct.mean).norm()>.01,"mixed fixture did not exercise indirect transfer");
        }
        s.rr_start=2;
        close_color(ray_image(r,q,gpu,s,4096,down_voxel).linear_rgb()[0],take(estimate_path_reference(oracle,down_voxel,s,{0,4096})).mean,.008);
    }
    // Mesh area emitters illuminate the same voxel BSDF through the existing MIS path.
    f.voxel.voxel->palette[0].metallic=0;
    for(auto& m:f.mesh.materials) {m.emissive={2,3,4};m.base_color={0,0,0,1};}
    s=settings(2); auto oracle=f.oracle();
    close_color(ray_image(r,q,take(r.prepare_scene(q,f.scene())),s,4096,down_voxel).linear_rgb()[0],take(estimate_path_reference(oracle,down_voxel,s,{0,4096})).mean,.006);
}
void voxel_state(memory::ResourceHandle heap,PathTraceRenderer& r,SubmissionQueue& q) {
    path_voxel_test::Fixture f{heap}; const std::array cube{assets::VoxelBlock{{0,0,0},1,1}}; f.grid(cube);
    auto cpu=f.scene(); auto gpu=take(r.prepare_scene(q,cpu)); auto view=f.view(); const auto original_view=view; auto s=settings(2); s.environment_rgb={.7,.8,.9};
    auto fixed=complete(q,take(r.submit_view(q,gpu,view,s,{0,7})));
    PathTraceSchedule schedule; schedule.target_spp=7; schedule.batch_spp=3; schedule.tile_edge=2; schedule.reverse_tiles=true;
    check(r.configure_view(q,239,gpu,view,s,schedule)); compare_images(finish_view(r,q,239),fixed,0);
    auto old=r.completed_frame(239); const auto generation=take(r.status(239)).generation;
    auto same=take(r.prepare_scene(q,f.scene())); require(same.submission().value()==gpu.submission().value(),"equal voxel geometry rebuilt AS");
    check(r.configure_view(q,239,gpu,view,s,schedule));
    require(take(r.status(239)).generation==generation,"unchanged voxel view reset history");
    auto medium=take(PathMediumSnapshot::create(f.document->environment()));
    require(!r.configure_medium_view(q,239,gpu,medium,view),"voxel medium accepted before M23.11");
    require(take(r.status(239)).generation==generation && take(r.status(239)).completed_spp==7,"unsupported mode damaged completed state");
    f.voxel.voxel->palette[0].base_color={.1f,.7f,.3f}; auto changed=take(r.prepare_scene(q,f.scene()));
    check(r.configure_view(q,239,changed,view,s,schedule)); require(take(r.status(239)).generation>generation && take(r.status(239)).completed_spp==0,"palette change retained old samples");
    auto palette=finish_view(r,q,239); require((palette.linear_rgb()[5]-fixed.linear_rgb()[5]).norm()>.01,"palette edit had no effect");
    compare_images(complete(q,take(old.read_linear(q))),fixed,0);
    const auto before=take(r.status(239)).generation;
    const std::array moved{assets::VoxelBlock{{2,0,0},1,1}}; f.grid(moved);
    fail_submit=true; require(!r.prepare_scene(q,f.scene()),"injected voxel AS submission failure accepted");
    require(!fail_submit,"failed prepare did not reach submit");
    require(take(r.status(239)).generation==before && take(r.status(239)).completed_spp==7,"failed upload damaged active history");
    auto occupancy=take(r.prepare_scene(q,f.scene())); check(r.configure_view(q,239,occupancy,view,s,schedule));
    require(take(r.status(239)).completed_spp==0,"occupancy change retained samples"); finish_view(r,q,239);
    Trsd moved_world; moved_world.translation={-2,0,0}; f.transform(moved_world); auto transformed=take(r.prepare_scene(q,f.scene()));
    view=f.view(); check(r.configure_view(q,239,transformed,view,s,schedule)); require(take(r.status(239)).completed_spp==0,"transform change retained samples");
    take(r.submit_batch(q,239)); check(r.release_view(239)); collect(r,q);
    compare_images(complete(q,take(old.read_linear(q))),fixed,0);
    // A rejected float transform must not poison the already prepared scene.
    Trsd far; far.translation={1e8,0,0}; f.transform(far); require(!r.prepare_scene(q,f.scene()),"unrepresentable voxel transform accepted");
    compare_images(complete(q,take(r.submit_view(q,gpu,original_view,s,{0,7}))),fixed,0);
}
} // namespace

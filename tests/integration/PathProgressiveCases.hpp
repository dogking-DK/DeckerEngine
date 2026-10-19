#pragma once
namespace {
bool collect(PathTraceRenderer& r,SubmissionQueue& q) {
    q.device().logical_device().waitIdle(); return take(r.poll(q));
}
void advance_view(PathTraceRenderer& r,SubmissionQueue& q,std::uint64_t id) {
    for(unsigned step=0;step<100000;++step) {
        const auto state=take(r.status(id));
        if(state.completed_spp>=state.target_spp && !state.pending)
            return;
        take(r.submit_batch(q,id)); collect(r,q);
    }
    throw std::runtime_error("progressive view failed to finish");
}
PathTraceGpuImage finish_view(PathTraceRenderer& r,SubmissionQueue& q,std::uint64_t id) {
    advance_view(r,q,id); return complete(q,take(r.completed_frame(id).read_linear(q)));
}
void compare_images(const PathTraceGpuImage& a,const PathTraceGpuImage& b,double tolerance=1e-6) {
    require(a.linear_rgb().size()==b.linear_rgb().size(),"progressive image size mismatch");
    for(std::size_t i=0;i<a.linear_rgb().size();++i) {
        close_color(a.linear_rgb()[i],b.linear_rgb()[i],tolerance);
        close_color(a.second_moment()[i],b.second_moment()[i],tolerance);
        require(bool(a.primary_hits()[i])==bool(b.primary_hits()[i]),"progressive primary hit changed");
        if(a.primary_hits()[i]) require(a.primary_hits()[i]->index==b.primary_hits()[i]->index,"first sample hit changed");
    }
    require(a.info().statistics.rays==b.info().statistics.rays,"progressive ray accounting changed");
}
void progressive_images(memory::ResourceHandle heap,PathTraceRenderer& r,SubmissionQueue& q) {
    Fixture f{heap}; f.quad(-.32f,.32f,0,{.6f,.4f,.2f}); f.quad(-.1f,.1f,.6f,{0,0,0},{2,3,4},true);
    auto cpu=f.scene(); auto gpu=take(r.prepare_scene(q,cpu)); auto view=f.view(f.source(),7);
    auto s=settings(3); s.environment_rgb={.2,.4,.6}; s.seed=723;
    auto once=complete(q,take(r.submit_view(q,gpu,view,s,{0,19})));
    PathTraceSchedule schedule; schedule.target_spp=19; schedule.batch_spp=3; schedule.tile_edge=4;
    check(r.configure_view(q,11,gpu,view,s,schedule));
    require(take(r.submit_batch(q,11)),"first tile not submitted");
    const auto submitted=q.stats().submitted;
    require(!take(r.submit_batch(q,11)) && q.stats().submitted==submitted,"duplicate interval submitted");
    require(take(r.status(11)).completed_spp==0,"submission prematurely advanced samples");
    require(!collect(r,q) && take(r.status(11)).completed_tiles==1,"partial tile published");
    auto a=finish_view(r,q,11); compare_images(a,once,0); // Same-device Kahan continuation is exact.
    auto reference=take(render_path_reference(heap,cpu,view,s,{0,19}));
    for(std::size_t i=0;i<reference.linear_rgb().size();++i) close_color(a.linear_rgb()[i],reference.linear_rgb()[i],2e-4);
    // Display uses the immutable completed HDR buffer; it must neither resample nor change history.
    const auto generation=take(r.status(11)).generation;
    for(const bool encoded:{false,true}) for(const double ev:{-2.,0.,2.}) {
        auto display=take(r.display(q,r.completed_frame(11),ev,encoded));
        require(take(q.wait(display.submission)),"tone submission timeout");
        auto batch=take(q.begin()); auto read=take(batch.readback(display.image,{0,0,0,0,7,7}));
        check(batch.transition(display.image,vk::ImageLayout::eShaderReadOnlyOptimal));
        require(take(q.wait(take(q.submit(std::move(batch))))),"tone readback timeout");
        std::array<std::byte,7*7*4> pixels{}; require(take(read.try_read(pixels)),"tone readback missing");
        for(std::size_t i=0;i<a.linear_rgb().size();++i) for(int channel=0;channel<3;++channel) {
            const double linear=a.linear_rgb()[i][channel]*std::exp2(ev); double expected=linear/(1+linear);
            if(encoded) expected=expected<=.0031308 ? 12.92*expected : 1.055*std::pow(expected,1./2.4)-.055;
            require(std::abs(int(std::to_integer<unsigned char>(pixels[i*4+channel]))-std::lround(expected*255))<=1,"tone/exposure/sRGB mismatch");
        }
    }
    require(take(r.status(11)).generation==generation && take(r.status(11)).completed_spp==19,"display changed history");
    require(!r.display(q,r.completed_frame(11),33),"invalid display exposure accepted");
    schedule.batch_spp=16; schedule.tile_edge=2;
    check(r.configure_view(q,22,gpu,view,s,schedule)); compare_images(finish_view(r,q,22),once,0);
    check(r.release_view(22)); check(r.reset(11));
    schedule.batch_spp=1; schedule.tile_edge=64; check(r.configure_view(q,11,gpu,view,s,schedule));
    compare_images(finish_view(r,q,11),once,0);
    // Large dispatches, non-divisible extents and reverse traversal keep sample identity.
    auto large_desc=view.description(); large_desc.width=257; large_desc.height=129;
    auto large=take(RenderView::create(f.source(),large_desc));
    schedule.target_spp=2; check(r.configure_view(q,11,gpu,large,s,schedule)); auto tiled=finish_view(r,q,11);
    check(r.reset(11)); schedule.tile_edge=256; schedule.reverse_tiles=true;
    check(r.configure_view(q,11,gpu,large,s,schedule)); compare_images(finish_view(r,q,11),tiled,0);
    schedule.tile_edge=257; require(!r.configure_view(q,11,gpu,large,s,schedule),"oversize dispatch accepted");
    require(take(r.status(11)).completed_spp==2,"invalid tile replaced completed frame"); schedule.reverse_tiles=false;
    // Extent boundary beyond the CPU oracle limit, including a short final tile.
    Fixture empty{heap}; auto sky=take(r.prepare_scene(q,empty.scene())); auto d=empty.view(empty.source()).description(); d.width=2048; d.height=2;
    auto wide=take(RenderView::create(empty.source(),d)); schedule.target_spp=2; schedule.batch_spp=2; schedule.tile_edge=63;
    check(r.configure_view(q,11,sky,wide,s,schedule)); auto wide_image=finish_view(r,q,11);
    require(wide_image.linear_rgb().size()==4096,"2048 extent rejected or truncated");
    for(const auto& color:wide_image.linear_rgb()) close_color(color,s.environment_rgb,1e-7);
    check(r.release_view(11));
    for(auto& material:f.asset.materials) { material.metallic=.7f; material.roughness=.35f; }
    const std::array assets{&f.asset}; auto ggx=take(PathTraceScene::create(heap,f.source(),assets,{},PathTraceMaterialModel::metallic_roughness));
    auto ggx_gpu=take(r.prepare_scene(q,ggx)); schedule.target_spp=19; schedule.batch_spp=7; schedule.tile_edge=3;
    check(r.configure_view(q,11,ggx_gpu,view,s,schedule));
    compare_images(finish_view(r,q,11),complete(q,take(r.submit_view(q,ggx_gpu,view,s,{0,19}))),0);
    schedule.target_spp=65536; schedule.batch_spp=16; schedule.tile_edge=1;
    auto single=empty.view(empty.source(),1); check(r.configure_view(q,11,sky,single,s,schedule));
    compare_images(finish_view(r,q,11),complete(q,take(r.submit_view(q,sky,single,s,{0,65536}))),0);
    std::puts("progressive unequal batches/tiles/global jitter/CPU/2048 boundary=pass");
}
void progressive_state(memory::ResourceHandle heap,PathTraceRenderer& r,SubmissionQueue& q) {
    Fixture f{heap}; f.quad(-1,1,0,{.5f,.5f,.5f}); auto cpu=f.scene(); auto gpu=take(r.prepare_scene(q,cpu));
    auto view=f.view(f.source()); auto s=settings(); s.environment_rgb=Vec3d::Ones();
    PathTraceSchedule schedule; schedule.target_spp=3; schedule.batch_spp=2;
    check(r.configure_view(q,1,gpu,view,s,schedule)); auto image=finish_view(r,q,1); auto held=r.completed_frame(1);
    const auto generation=take(r.status(1)).generation; const auto submitted=q.stats().submitted;
    schedule.exposure=2; schedule.target_spp=1; schedule.batch_spp=1; schedule.tile_edge=2;
    auto d=view.description(); ++d.frame; auto next=take(RenderView::create(f.source(),d));
    check(r.configure_view(q,1,gpu,next,s,schedule));
    require(take(r.status(1)).generation==generation && take(r.status(1)).completed_spp==3,"display/schedule reset history");
    require(!take(r.submit_batch(q,1)) && q.stats().submitted==submitted,"lower target resampled");
    compare_images(complete(q,take(r.completed_frame(1).read_linear(q))),image,0);
    schedule.target_spp=5; schedule.paused=true; check(r.configure_view(q,1,gpu,next,s,schedule));
    require(!take(r.submit_batch(q,1)),"paused view submitted"); schedule.paused=false; check(r.configure_view(q,1,gpu,next,s,schedule));
    auto continued=finish_view(r,q,1); require(continued.info().samples.count==5,"resume lost sample interval");
    check(r.configure_view(q,2,gpu,view,s,schedule)); require(!r.configure_view(q,3,gpu,view,s,schedule),"third view accepted");
    take(r.submit_batch(q,2)); require(!take(r.submit_batch(q,1)),"two batches in flight");
    check(r.reset(2)); require(!collect(r,q) && !r.completed_frame(2),"reset accepted late completion");
    for(int kind=0;kind<8;++kind) {
        auto before=take(r.status(1)).generation;
        if(kind==0) { Trsd camera; camera.translation={.05,0,1}; d.camera_world=Transformd::from_trs(camera).value(); }
        if(kind==1) d.projection(0,0)*=1.01;
        if(kind==2) d.width=5;
        if(kind==3) ++s.seed;
        if(kind==4) ++schedule.gesture;
        if(kind==5) ++schedule.session;
        if(kind==6) { f.asset.materials[0].base_color[0]=.25f; gpu=take(r.prepare_scene(q,f.scene())); }
        if(kind==7) { Trsd transform; transform.translation={.1,0,0}; f.document->set_local_transform(f.entity,transform).value(); gpu=take(r.prepare_scene(q,f.scene())); }
        next=take(RenderView::create(f.source(),d)); check(r.configure_view(q,1,gpu,next,s,schedule));
        require(take(r.status(1)).generation!=before && take(r.status(1)).completed_spp==0,"integrator key failed to reset");
        finish_view(r,q,1);
    }
    check(r.release_view(2)); require(!r.status(2),"released view visible");
    require(held.info().samples.count==3,"external completed owner mutated");
    std::puts("progressive display/pause/target/two views/reset/asset/camera/gesture/session=pass");
}
void progressive_failure(memory::ResourceHandle heap,PathTraceRenderer& r,SubmissionQueue& q) {
    Fixture f{heap}; auto gpu=take(r.prepare_scene(q,f.scene())); auto view=f.view(f.source(),5); auto s=settings(); s.environment_rgb={.1,.2,.3};
    PathTraceSchedule schedule; schedule.target_spp=1; schedule.tile_edge=3;
    check(r.configure_view(q,7,gpu,view,s,schedule)); auto old=finish_view(r,q,7);
    schedule.target_spp=2; check(r.configure_view(q,7,gpu,view,s,schedule));
    take(r.submit_batch(q,7)); require(!collect(r,q),"first tile published whole batch");
    fail_submit=true; require(!r.submit_batch(q,7),"injected later tile submit succeeded");
    require(take(r.status(7)).completed_spp==1 && !take(r.status(7)).pending,"failed batch advanced state");
    compare_images(complete(q,take(r.completed_frame(7).read_linear(q))),old,0);
    auto retry=finish_view(r,q,7); require(retry.info().samples.count==2,"retry lost interval");
    schedule.target_spp=3; check(r.configure_view(q,7,gpu,view,s,schedule));
    std::stop_source stop; take(r.submit_batch(q,7,stop.get_token())); timeout_once=true;
    require(!take(r.poll(q)) && take(r.status(7)).completed_spp==2,"timeout published");
    stop.request_stop(); require(!collect(r,q) && take(r.status(7)).completed_spp==2,"cancel advanced samples");
    take(r.submit_batch(q,7)); check(r.cancel(7)); require(!collect(r,q),"explicit cancel published");
    require(take(r.status(7)).completed_spp==2,"cancel erased completed history");
    take(r.submit_batch(q,7)); check(r.release_view(7)); check(r.configure_view(q,7,gpu,view,s,schedule));
    require(!collect(r,q) && take(r.status(7)).completed_spp==0,"reused id accepted obsolete batch");
    auto a=finish_view(r,q,7); auto once=complete(q,take(r.submit_view(q,gpu,view,s,{0,3}))); compare_images(a,once,0);
    auto invalid=schedule; invalid.batch_spp=17; const auto generation=take(r.status(7)).generation;
    require(!r.configure_view(q,7,gpu,view,s,invalid),"invalid batch accepted");
    require(take(r.status(7)).generation==generation && take(r.status(7)).completed_spp==3,"invalid config changed state");
    schedule.target_spp=4; check(r.configure_view(q,7,gpu,view,s,schedule)); take(r.submit_batch(q,7));
    // Poll must work with every queue slot occupied; it cannot allocate a dummy command batch.
    auto first=q.begin(); auto second=q.begin(); require(bool(first) && bool(second),"expected two spare slots");
    q.device().logical_device().waitIdle(); require(!take(r.poll(q)),"partial batch published with occupied slots");
    first=CommandBatch{}; second=CommandBatch{};
    std::stop_source already; already.request_stop(); require(!r.submit_batch(q,7,already.get_token()),"pre-cancelled tile submitted");
    require(!take(r.poll(q)) && take(r.status(7)).completed_spp==3,"pre-cancelled batch published");
    std::puts("progressive later-tile failure/retry/timeout/cancel/late release/config protection=pass");
}
void progressive_budget(memory::ResourceHandle heap,PathTraceRenderer&,SubmissionQueue& q) {
    const auto recommended=PathTraceRenderer::recommended_memory_limit(q.device());
    require(recommended>=(1ull<<20) && recommended<=PathTraceRenderer::max_memory_limit,"invalid device budget recommendation");
    require(!PathTraceRenderer::create(heap,q,DK_PATH_SHADER_DIR,{},PathTraceRenderer::max_memory_limit+1),"oversize GPU budget accepted");
    // Generic budget survives wrapper destruction until pending uses retire, including AS scratch.
    auto budget=memory::make_shared_in<AllocationBudget>(heap,1ull<<20); auto factory=q.resources().with_budget(budget);
    auto source=take(factory.create_buffer({4096,vk::BufferUsageFlagBits::eTransferSrc,BufferMemory::upload}));
    std::array<std::byte,4096> bytes{}; check(source.write(0,bytes));
    auto destination=take(factory.create_buffer({4096,vk::BufferUsageFlagBits::eTransferDst,BufferMemory::readback}));
    auto batch=take(q.begin()); check(batch.copy(source,destination,4096)); auto ticket=take(q.submit(std::move(batch)));
    auto charged=budget->stats().current; source={}; destination={};
    require(budget->stats().current==charged && charged>=8192,"pending budget released early");
    require(take(q.wait(ticket)),"budget copy wait"); require(budget->stats().current==0,"completed budget not reclaimed");
    require(!factory.create_buffer({2ull<<20}),"over-budget allocation accepted");
    require(budget->stats().current==0,"budget rejection leaked reservation");
    auto r=take(PathTraceRenderer::create(heap,q,DK_PATH_SHADER_DIR,{},1ull<<20));
    const auto base=r.memory_stats(q).owned.current;
    Fixture geometry{heap}; geometry.quad(-1,1,0,{.5f,.5f,.5f});
    { auto scene=take(r.prepare_scene(q,geometry.scene())); const auto prepared=r.memory_stats(q).owned.current;
      require(prepared>base,"AS resources not charged"); require(take(q.wait(scene.submission())),"AS budget wait failed");
      require(r.memory_stats(q).owned.current<prepared,"AS scratch/upload charge not reclaimed"); }
    require(r.memory_stats(q).owned.current==base,"AS retained charges after destruction");
    Fixture f{heap}; auto gpu=take(r.prepare_scene(q,f.scene())); auto view=f.view(f.source(),48); auto s=settings(); s.environment_rgb=Vec3d::Ones();
    PathTraceSchedule schedule; schedule.target_spp=1;
    check(r.configure_view(q,1,gpu,view,s,schedule)); finish_view(r,q,1);
    auto held=r.completed_frame(1); const auto one=r.memory_stats(q).owned.current;
    check(r.reset(1)); require(r.memory_stats(q).owned.current==one,"external frame charge lost on reset");
    finish_view(r,q,1); require(r.memory_stats(q).owned.current>=one+48*48*128,"old and new owners not charged");
    auto second=r.completed_frame(1); check(r.reset(1)); advance_view(r,q,1);
    // Three retained full histories fit, fourth candidate does not; readback may also hit the same budget.
    schedule.target_spp=2; check(r.configure_view(q,1,gpu,view,s,schedule));
    require(!r.submit_batch(q,1) && take(r.status(1)).completed_spp==1,"history budget failure advanced spp");
    held={}; second={}; require(r.memory_stats(q).owned.current<2*48*48*128,"external owners did not release budget");
    finish_view(r,q,1);
    auto huge=schedule; huge.target_spp=65536; auto d=view.description(); d.width=d.height=2048;
    require(!r.configure_view(q,1,gpu,take(RenderView::create(f.source(),d)),s,huge),"target work overflow accepted");
    auto throttled=s; throttled.max_rays=12; schedule.target_spp=2; schedule.batch_spp=2; schedule.tile_edge=64;
    check(r.configure_view(q,2,gpu,f.view(f.source(),5),throttled,schedule)); take(r.submit_batch(q,2));
    require(take(r.status(2)).total_tiles==25,"work budget did not subdivide tiles");
    finish_view(r,q,2);
    auto stats=r.memory_stats(q); require(stats.owned.peak<=stats.owned.limit && stats.device_block_bytes>=stats.device_allocation_bytes,"memory report inconsistent");
    std::printf("budget current=%llu peak=%llu host=%llu device=%llu VMA allocation=%llu block=%llu\n",
        stats.owned.current,stats.owned.peak,stats.owned.host_visible,stats.owned.device_local,stats.device_allocation_bytes,stats.device_block_bytes);
    std::puts("progressive work/memory/external owner/pending/recovery=pass");
}
void progressive_lost(memory::ResourceHandle heap,Device& device) {
    auto q=take(graphics::detail::SubmissionAccess::create(heap,take(device.share_queue(0)),3,{submit_override,nullptr,wait_override}));
    {
        auto r=take(PathTraceRenderer::create(heap,q,DK_PATH_SHADER_DIR));
        Fixture f{heap}; auto gpu=take(r.prepare_scene(q,f.scene())); auto view=f.view(f.source());
        PathTraceSchedule schedule; schedule.target_spp=1;
        check(r.configure_view(q,1,gpu,view,settings(),schedule)); finish_view(r,q,1); auto old=r.completed_frame(1);
        schedule.target_spp=2; check(r.configure_view(q,1,gpu,view,settings(),schedule)); take(r.submit_batch(q,1));
        // Complete real hardware work before injecting a synthetic terminal loss.
        q.device().logical_device().waitIdle(); lose_wait=true;
        require(!r.poll(q) && take(r.status(1)).device_lost && take(r.status(1)).completed_spp==1,"device loss published history");
        require(!r.submit_batch(q,1) && !old.read_linear(q),"lost session accepted GPU work");
    }
    require(!q.close() && q.stats().closed,"lost queue did not report close error");
    std::puts("progressive terminal device loss/old history/no retry=pass");
}
}

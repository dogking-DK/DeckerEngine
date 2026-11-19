#include "PathRestirProbe.hpp"
#include "../RestirSpatialOracle.hpp"

namespace dk::test::path_probe {
void restir_direct(memory::ResourceHandle heap,PathTraceRenderer& r,SubmissionQueue& q) {
    Fixture f{heap}; f.quad(-8,8,0,{.6f,.4f,.2f});
    auto gpu=take(r.prepare_scene(q,f.scene())); auto s=settings(); s.environment_rgb=Vec3d::Ones();
    auto ris=surface_direct_restir_settings(); ris.spatial.reset();
    auto view=temporal_view(f,1,false,.3,0,.05);
    PathTraceSchedule schedule; schedule.target_spp=64; schedule.batch_spp=16;
    check(r.configure_restir_view(q,182,gpu,view,s,ris,schedule)); auto a=finish_view(r,q,182);
    close_color(a.linear_rgb()[0],temporal_furnace_oracle(s.seed,64,ris,Vec3f{.6f,.4f,.2f}.cast<double>()),1e-5);
    require(a.info().temporal_statistics.expired==0 && a.info().temporal_statistics.max_age==63 &&
        a.info().temporal_statistics.max_candidates==21*32,"paper history ratio or lifetime differs");
    auto moved=temporal_view(f,1,false,.30001,0,.05); schedule.target_spp=1;
    check(r.configure_restir_view(q,182,gpu,moved,s,ris,schedule)); auto motion=finish_view(r,q,182);
    std::printf("direct motion epoch=%u reused=%llu rejected=%llu\n",motion.info().sampling_first_epoch,(unsigned long long)motion.info().temporal_statistics.reused,(unsigned long long)motion.info().temporal_statistics.rejected);
    require(motion.info().sampling_first_epoch==64 && motion.info().temporal_statistics.reused==1,"direct camera history lost");
    const auto held=r.completed_frame(182).linear_buffer(); auto invalid=s; invalid.max_bounces=2;
    require(!r.configure_restir_view(q,182,gpu,moved,invalid,ris,schedule) && r.completed_frame(182).linear_buffer()==held,"direct invalid depth changed publication");
    auto wrong=ris; wrong.temporal->version=1;
    require(!r.configure_restir_view(q,182,gpu,moved,s,wrong,schedule),"mixed reservoir versions accepted");
    check(r.reset(182)); auto reset=finish_view(r,q,182);
    require(reset.info().sampling_first_epoch==0 && reset.info().temporal_statistics.reused==0,"direct reset retained history");
    check(r.release_view(182));

    // Independent CPU geometry/visibility + double-precision support-matrix oracle.
    // An overhead occluder makes visible support differ between adjacent receivers.
    f.quad(-.25f,.25f,1.5f,{0,0,0},{0,0,0},true);
    gpu=take(r.prepare_scene(q,f.scene())); view=temporal_view(f,5,true,0,0,2);
    ris=surface_direct_restir_settings(); ris.temporal.reset(); ris.spatial->radius=2;
    schedule.target_spp=64; schedule.batch_spp=16;
    check(r.configure_restir_view(q,182,gpu,view,s,ris,schedule)); auto visible=finish_view(r,q,182);
    const auto oracle=restir_spatial_oracle::render(gpu.source(),view,s,ris,64);
    for(unsigned i=0;i<25;++i) close_color(visible.linear_rgb()[i],oracle[i],2e-5);
    require(visible.info().spatial_statistics.support_corrected>0 && visible.info().spatial_statistics.current_visibility_rays>25*64,
        "visibility correction fixture inactive");
    check(r.release_view(182));
    ris.temporal=surface_direct_restir_settings().temporal;
    schedule.target_spp=19; PathTraceGpuImage baseline;
    for(unsigned batch:{1u,7u,16u}) {
        schedule.batch_spp=batch; schedule.tile_edge=batch==1 ? 64 : 3; schedule.reverse_tiles=batch==16;
        check(r.configure_restir_view(q,182,gpu,view,s,ris,schedule)); auto actual=finish_view(r,q,182);
        if(baseline) { compare_images(actual,baseline,0); compare_restir_statistics(actual.info(),baseline.info()); }
        else baseline=actual;
        check(r.release_view(182));
    }
    std::puts("paper RGB direct visibility/support oracle, history, reset, failure and scheduling=pass");
}
} // namespace dk::test::path_probe

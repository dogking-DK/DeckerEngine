#pragma once
namespace {
void restir_medium_support(memory::ResourceHandle heap,PathTraceRenderer& r,SubmissionQueue& q,bool initial=false) {
    Fixture f{heap}; auto e=cloud_environment(); e.atmosphere.ground_albedo={.2,.6,.8}; check(f.document->set_environment(e));
    // Adjacent ground/mesh receivers have different colors/normals. A one-sided area
    // emitter crosses support between receivers; a mask wall changes visibility only.
    f.triangle({0,.2f,-1},{1,.5f,1},{1,.5f,-1},{.7f,.3f,.2f});
    f.triangle({0,.2f,-1},{0,.2f,1},{1,.5f,1},{.7f,.3f,.2f});
    f.triangle({0,1,-1},{0,3,-1},{0,1,1},{0,0,0},{4,3,2});
    f.triangle({.2f,.3f,-.5f},{.2f,1,-.5f},{.2f,.3f,.5f},{0,0,0},{0,0,0},true);
    f.asset.materials.back().alpha_mode=AlphaMode::mask;
    auto gpu=take(r.prepare_scene(q,f.scene())); CloudFieldCache cache; PathMediumSettings ms; ms.time_seconds=17; ms.cloud_albedo=0;
    auto m=take(PathMediumSnapshot::create(e,ms,take(cache.prepare(e.clouds)))); auto view=medium_down_view(f,3,true,4,0);
    auto s=air_settings(); s.environment_rgb={.2,.3,.4}; auto ris=spatial_settings(true); ris.spatial->neighbor_count=8;
    ris.spatial->radius=1; ris.spatial->normal_cosine=-1; ris.spatial->max_source_candidates=8;
    ris.temporal->max_history_age=4; ris.temporal->max_history_candidates=4;
    std::array<std::array<Vec3d,8>,9> observations,reference,short_chain,initial_reference; std::uint64_t corrected=0,reused=0;
    for(unsigned chain=0;chain<8;++chain) {
        s.seed=101+7919*chain; PathTraceSchedule schedule; schedule.target_spp=8192; schedule.batch_spp=16;
        if(initial) {
            auto image=complete(q,take(r.submit_restir_medium_view(q,gpu,m,view,s,{}, {0,16384})));
            auto classic=take(render_volume_path_reference(heap,gpu.source(),m,view,s,{0,65536}));
            auto cpu_ris=take(render_restir_volume_reference(heap,gpu.source(),m,view,s,{}, {0,16384}));
            for(unsigned pixel=0;pixel<9;++pixel) {
                observations[pixel][chain]=image.linear_rgb()[pixel]; reference[pixel][chain]=classic.linear_rgb()[pixel];
                initial_reference[pixel][chain]=cpu_ris.linear_rgb()[pixel];
            }
            continue;
        }
        check(r.configure_restir_medium_view(q,127,gpu,m,view,s,ris,schedule)); auto low=finish_view(r,q,127);
        for(unsigned pixel=0;pixel<9;++pixel) short_chain[pixel][chain]=low.linear_rgb()[pixel];
        schedule.target_spp=32768;
        check(r.configure_restir_medium_view(q,127,gpu,m,view,s,ris,schedule)); auto image=finish_view(r,q,127);
        corrected+=image.info().spatial_statistics.support_corrected; reused+=image.info().spatial_statistics.reused;
        auto cpu=take(render_volume_path_reference(heap,gpu.source(),m,view,s,{0,65536}));
        for(unsigned pixel=0;pixel<9;++pixel) { observations[pixel][chain]=image.linear_rgb()[pixel]; reference[pixel][chain]=cpu.linear_rgb()[pixel]; }
        check(r.release_view(127));
    }
    if(initial) {
        for(unsigned pixel=0;pixel<9;++pixel) {
            std::printf("medium support initial CPU RIS pixel=%u\n",pixel); temporal_mean(observations[pixel],initial_reference[pixel]);
        }
        for(unsigned pixel=0;pixel<9;++pixel) {
            std::printf("medium support initial classic pixel=%u\n",pixel); temporal_mean(observations[pixel],reference[pixel]);
        }
        return;
    }
    double low_mse=0,high_mse=0;
    for(unsigned pixel=0;pixel<9;++pixel) for(unsigned chain=0;chain<8;++chain) {
        low_mse+=(short_chain[pixel][chain]-reference[pixel][chain]).squaredNorm()/216;
        high_mse+=(observations[pixel][chain]-reference[pixel][chain]).squaredNorm()/216;
    }
    std::printf("medium support N MSE=%.9g 4N MSE=%.9g corrected=%llu reused=%llu\n",low_mse,high_mse,corrected,reused);
    require(high_mse<low_mse,"ground/mesh support chain did not converge");
    bool matched=true;
    for(unsigned pixel=0;pixel<9;++pixel) {
        std::printf("medium support pixel=%u\n",pixel);
        try { temporal_mean(observations[pixel],reference[pixel]); } catch(const std::runtime_error&) { matched=false; }
    }
    require(matched,"medium support independent-chain means differ");
    require(corrected>0 && reused>1000,"ground/mesh support correction fixture inactive");
    std::printf("medium support ground/mesh/one-sided emitter/mask/cloud corrected=%llu reused=%llu=pass\n",corrected,reused);
}
}

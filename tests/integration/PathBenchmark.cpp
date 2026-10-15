#include "PathBenchmarkSupport.hpp"

namespace {
void identical(std::span<const Vec3d> a,std::span<const Vec3d> b) {
    require(a.size()==b.size(),"Dynamic image size mismatch");
    for(std::size_t i=0;i<a.size();++i) require((a[i]-b[i]).cwiseAbs().maxCoeff()<=1e-6,"Dynamic reset differs from fresh view");
}
void dynamic(Fixture& f,PathTraceRenderer& r,SubmissionQueue& q,const PathTraceGpuScene& gpu,Json& out) {
    auto settings=f.settings(); settings.seed=101;
    PathTraceSchedule schedule; schedule.target_spp=4; schedule.batch_spp=4;
    check(r.configure_view(q,1,gpu,f.view(),settings,schedule)); advance(r,q,1,false);
    auto original=read_image(q,take(r.completed_frame(1).read_linear(q)));
    check(r.reset(1)); require(take(r.status(1)).completed_spp==0,"Reset retained samples"); advance(r,q,1,false);
    identical(original.linear_rgb(),read_image(q,take(r.completed_frame(1).read_linear(q))).linear_rgb());
    check(r.configure_view(q,1,gpu,f.view(4,4,.1),settings,schedule)); require(take(r.status(1)).completed_spp==0,"Camera did not reset");
    advance(r,q,1,false); auto changed=read_image(q,take(r.completed_frame(1).read_linear(q)));
    check(r.configure_view(q,2,gpu,f.view(4,4,.1),settings,schedule)); advance(r,q,2,false);
    identical(changed.linear_rgb(),read_image(q,take(r.completed_frame(2).read_linear(q))).linear_rgb());
    schedule.target_spp=8; check(r.configure_view(q,1,gpu,f.view(4,4,.1),settings,schedule));
    require(take(r.submit_batch(q,1)),"Cancellation tile not submitted"); const auto pending=r.pending_submission(); check(r.cancel(1));
    require(take(q.wait(pending)),"Cancelled GPU wait failed"); require(!take(r.poll(q)),"Cancelled tile published");
    require(take(r.status(1)).completed_spp==4,"Cancellation advanced sample count");
    check(r.reset(1)); schedule.target_spp=4; check(r.configure_view(q,1,gpu,f.view(4,4,.1),settings,schedule)); advance(r,q,1,false);
    identical(changed.linear_rgb(),read_image(q,take(r.completed_frame(1).read_linear(q))).linear_rgb());
    out["dynamic"]={{"reset_fresh",true},{"camera_fresh",true},{"pending_cancel_recovery",true}};
    check(r.release_view(1)); check(r.release_view(2));
}
void quality(Fixture& f,PathTraceRenderer& r,SubmissionQueue& q,const PathTraceGpuScene& gpu,Json& out) {
    out["seeds"]=Json::array();
    for(unsigned i=0;i<8;++i) {
        auto settings=f.settings(); settings.seed=101+7919*i;
        Json sample={{"seed",settings.seed},{"cpu_n_spp",16384},{"cpu_4n_spp",65536},{"gpu_spp",65536}};
        const auto start=Clock::now();
        const auto n=take(render_path_reference(f.heap,f.cpu,f.view(),settings,{0,16384}));
        const auto four=take(render_path_reference(f.heap,f.cpu,f.view(),settings,{0,65536}));
        sample["cpu_ms"]=elapsed(start); sample["cpu_n"]=pixels(n.linear_rgb()); sample["cpu_4n"]=pixels(four.linear_rgb());
        sample["gpu_4n"]=pixels(read_image(q,take(r.submit_view(q,gpu,f.view(),settings,{0,65536}))).linear_rgb());
        sample["progressive"]=Json::array();
        for(const unsigned target:{64u,256u,1024u}) {
            PathTraceSchedule schedule; schedule.target_spp=target; schedule.batch_spp=16;
            check(r.configure_view(q,1,gpu,f.view(),settings,schedule)); auto run=advance(r,q,1,false);
            run["linear"]=pixels(read_image(q,take(r.completed_frame(1).read_linear(q))).linear_rgb()); run.erase("tiles");
            sample["progressive"].push_back(std::move(run));
        }
        out["seeds"].push_back(std::move(sample));
        std::printf("quality %s seed %u complete\n",out["input"]["name"].get<std::string>().c_str(),i); std::fflush(stdout);
    }
    dynamic(f,r,q,gpu,out);
}
void performance(Fixture& f,PathTraceRenderer& r,SubmissionQueue& q,const PathTraceGpuScene& gpu,Json& out,bool smoke) {
    const unsigned width=smoke ? 160 : 1920,height=smoke ? 90 : 1080,frames=smoke ? 2 : 7;
    auto settings=f.settings(); settings.seed=101; settings.max_bounces=8;
    out["workload"]={{"width",width},{"height",height},{"max_bounces",8},{"batch_spp",1},{"tile_edge",64},{"frames",frames}};
    out["frames"]=Json::array();
    for(unsigned i=0;i<frames;++i) {
        PathTraceSchedule schedule; schedule.target_spp=i+1;
        check(r.configure_view(q,1,gpu,f.view(width,height),settings,schedule));
        auto run=advance(r,q,1,true); run["index"]=i; run["phase"]=i ? "steady" : "cold";
        run["memory"]=memory(r,q); out["frames"].push_back(std::move(run));
    }
    {
        const auto start=Clock::now(); auto display=take(r.display(q,r.completed_frame(1),0,true));
        require(take(q.wait(display.submission)),"Tone wait failed");
        out["tone"]={{"wall_ms",elapsed(start)},{"gpu",profile(display.submission,true)}};
    }
    {
        const auto start=Clock::now(); auto read=take(r.completed_frame(1).read_linear(q)); auto image=read_image(q,read);
        out["linear_readback"]={{"wall_ms",elapsed(start)},{"gpu",profile(read.submission(),true)},{"samples",image.info().samples.count}};
    }
    {
        auto held=r.completed_frame(1); const auto before=memory(r,q); PathTraceSchedule schedule; schedule.target_spp=frames+1;
        check(r.configure_view(q,1,gpu,f.view(width,height),settings,schedule)); require(take(r.submit_batch(q,1)),"Pending memory tile missing");
        out["memory_steady"]=before; out["memory_inflight_with_old"]=memory(r,q);
        const auto pending=r.pending_submission(); const auto start=Clock::now(); check(r.cancel(1)); out["cancel_call_ms"]=elapsed(start);
        require(take(q.wait(pending)),"Cancel wait failed"); require(!take(r.poll(q)),"Cancel published tile"); out["cancel_reclaim_ms"]=elapsed(start);
        require(take(r.status(1)).completed_spp==frames,"Cancel altered completed image");
    }
    check(r.release_view(1));
    // Actual time to a declared quality level is measured on the same 4x4 protocol.
    out["quality_ladder"]=Json::array();
    for(unsigned seed=0;seed<8;++seed) {
        auto s=f.settings(); s.seed=101+7919*seed; Json item={{"seed",s.seed},{"levels",Json::array()}};
        const auto start=Clock::now();
        for(unsigned target:{64u,256u,1024u}) {
            PathTraceSchedule schedule; schedule.target_spp=target; schedule.batch_spp=16;
            check(r.configure_view(q,1,gpu,f.view(),s,schedule)); advance(r,q,1,false);
            auto image=read_image(q,take(r.completed_frame(1).read_linear(q)));
            item["levels"].push_back({{"spp",target},{"cumulative_ms",elapsed(start)},{"linear",pixels(image.linear_rgb())}});
        }
        out["quality_ladder"].push_back(std::move(item));
    }
    check(r.release_view(1)); out["memory_after_views"]=memory(r,q);
}
void run(Json& output,bool is_quality,bool smoke) {
    auto system=take(memory::MemorySystem::create()); auto heap=take(system.create_heap({"path-benchmark",memory::DomainCategory::render})); Diagnostics diagnostics;
    {
        memory::ThreadContext context{system,heap}; memory::ExecutionScope scope{context,heap};
        DeviceOptions options; options.validation=is_quality ? ValidationMode::required : ValidationMode::disabled;
        options.ray_tracing=RayTracingMode::required; options.sampled_image_indexing=SampledImageIndexingMode::required;
        options.diagnostic_sink=diagnostic; options.diagnostic_user_data=&diagnostics;
        const auto start=Clock::now(); auto q=take(SubmissionQueue::create(heap,take(Device::create(heap,options)))); output["device_queue_ms"]=elapsed(start);
        check(q.configure_gpu_profiling({!is_quality,64}));
        const auto& a=q.device().adapter(); output["device"]={{"name",a.properties.deviceName.data()},{"driver",a.driver.driverInfo.data()}};
        {
            const auto compile=Clock::now(); auto renderer=take(PathTraceRenderer::create(heap,q,DK_PATH_SHADER_DIR)); output["compile_pipeline_ms"]=elapsed(compile);
            for(const auto* name:{"area-lambert","ggx-box","disk-mask"}) {
                const auto prepare=Clock::now(); Fixture fixture{heap,name};
                Json item={{"input",fixture.metadata(name)},{"cpu_input_ms",elapsed(prepare)}};
                const auto upload=Clock::now(); auto gpu=take(renderer.prepare_scene(q,fixture.cpu));
                if(gpu.submission().value()) require(take(q.wait(gpu.submission())),"Scene build wait failed");
                item["upload_build_ms"]=elapsed(upload); item["upload_build_gpu"]=profile(gpu.submission(),!is_quality);
                if(is_quality) quality(fixture,renderer,q,gpu,item); else performance(fixture,renderer,q,gpu,item,smoke);
                output["cases"].push_back(std::move(item));
            }
        }
        check(q.poll()); check(q.close()); VmaTotalStatistics statistics{}; vmaCalculateStatistics(q.device().allocator(),&statistics);
        output["vma_live_allocations"]=statistics.total.statistics.allocationCount;
        require(!statistics.total.statistics.allocationCount,"GPU allocation leak");
    }
    output["cpu_peak_bytes"]=heap.snapshot().peak_backing_bytes; output["cpu_live_allocations"]=heap.snapshot().live_allocations;
    output["diagnostics"]={{"errors",diagnostics.errors.load()},{"warnings",diagnostics.warnings.load()}};
    require(!diagnostics.errors && !diagnostics.warnings && !heap.snapshot().live_allocations,"Diagnostics or CPU memory leak");
    heap.begin_close(); require(system.try_close().closed(),"Memory close failed");
}
}
int main(int argc,char** argv) {
    Json output={{"schema",1},{"status","failed"},{"cases",Json::array()}}; std::filesystem::path report;
    try {
        bool quality=false,smoke=false;
        for(int i=1;i<argc;++i) {
            const std::string_view option=argv[i];
            if(option=="--quality") quality=true;
            else if(option=="--smoke") smoke=true;
            else if(option=="--output" && i+1<argc) report=argv[++i];
            else throw std::runtime_error("Unknown path benchmark option");
        }
        require(!report.empty() && !(quality && smoke),"Output and one benchmark mode required");
        std::filesystem::create_directories(report.parent_path());
        output.update({{"kind",quality ? "quality" : "performance"},{"validation",quality},{"timestamps",!quality},
            {"smoke",smoke},{"configuration",DK_BENCH_CONFIG},{"compiler",DK_BENCH_COMPILER},
            {"tracy_compiled",profiling::enabled()},{"tracy_connected",profiling::is_connected()}});
        run(output,quality,smoke); output["status"]="passed";
    } catch(const std::exception& error) { output["error"]=error.what(); std::fprintf(stderr,"Path benchmark: %s\n",error.what()); }
    if(!report.empty()) { std::ofstream file(report); file<<output.dump(2)<<'\n'; if(!file) return 1; }
    return output["status"]=="passed" ? 0 : 1;
}

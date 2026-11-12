#include "VoxelBenchmarkCases.hpp"
#include <algorithm>
#include <cstdio>

namespace {
void run(Json& output,bool smoke,bool validation,unsigned samples,unsigned round,const std::filesystem::path& path) {
    openvdb::initialize(); auto memory=take(memory::MemorySystem::create()); auto heap=take(memory.create_heap({"voxel-baseline",memory::DomainCategory::render})); Diagnostics diagnostics;
    {
        memory::ThreadContext context{memory}; memory::ExecutionScope scope{context,heap}; DeviceOptions options;
        options.validation=validation ? ValidationMode::required : ValidationMode::disabled; options.diagnostic_sink=diagnostic; options.diagnostic_user_data=&diagnostics;
        auto start=Clock::now(); auto q=take(SubmissionQueue::create(heap,take(Device::create(heap,options)),3)); check(q.configure_gpu_profiling({true})); output["device_queue_ms"]=ms(start);
        require(!q.device().ray_tracing_enabled(),"raster benchmark must not require RT"); const auto& a=q.device().adapter();
        output["device"]={{"name",a.properties.deviceName.data()},{"driver",a.driver.driverInfo.data()},{"vendor_id",a.properties.vendorID},{"device_id",a.properties.deviceID}};
        output["device_only"]=gpu_memory(q);
        {
            start=Clock::now(); auto tuned=take(VoxelGpuQuery::create(heap,q)); auto baseline=variant(heap,q,path.parent_path(),"baseline-shader","DK_VOXEL_BASELINE_TRANSITION");
            auto scalar=variant(heap,q,path.parent_path(),"scalar-shader","DK_VOXEL_DISABLE_HDDA"); auto pipeline=take(ScenePipeline::create(heap,q,DK_RENDER_SHADER_DIR));
            output["pipeline_create_ms"]=ms(start); output["query_reflection"]=Json::parse(tuned.shader_reflection());
            auto meshes=take(GpuAssets::create(heap)); auto document=take(SceneDocument::create()); auto scene=take(RenderScene::extract(heap,take(document->snapshot())));
            if(!smoke) {
                // Keep first-use/JIT/power-state ramp outside the per-case samples,
                // but retain every preconditioning frame and its startup cost.
                const Input warm{"device_precondition",0,{{0,0,0},{63,63,63}},{{{0,0,0},8}}};
                assets::VoxelBuildOptions build; build.domain=warm.domain;
                auto raw=source(warm); auto cpu=take(assets::detail::convert_openvdb(*raw,build)); auto cache=take(VoxelGpuCache::create(q)); auto grid=take(cache->prepare(q,cpu));
                require(take(q.wait(grid->upload_submission())),"precondition upload incomplete");
                output["precondition"]=render_case(warm,cpu,{},heap,q,pipeline,meshes,scene,grid,1280,720,false,61);
            }
            check(q.poll());
            output["cases"]=Json::array();
            for(const auto& in:inputs(smoke)) {
                std::printf("voxel baseline %s\n",in.name.c_str()); std::fflush(stdout); Json row={{"name",in.name},{"domain_min",in.domain.minimum},{"domain_max",in.domain.maximum}};
                const auto before=gpu_memory(q);
                {
                    MemorySampler sampler; start=Clock::now(); auto raw=source(in); row["source_build_ms"]=ms(start);
                    assets::VoxelBuildOptions build; build.domain=in.domain; start=Clock::now(); auto cpu=take(assets::detail::convert_openvdb(*raw,build)); row["conversion_ms"]=ms(start);
                    row["conversion_process_memory"]=sampler.finish(); const auto& stats=cpu.statistics(); const auto* nano=reinterpret_cast<const nanovdb::NanoGrid<std::int32_t>*>(cpu.bytes().data());
                    row["storage"]={{"source_tree_bytes",stats.source_tree_bytes},{"normalized_tree_bytes",stats.normalized_tree_bytes},{"nano_bytes",stats.nano_bytes},
                        {"conversion_live_payload_bytes",stats.source_tree_bytes+stats.normalized_tree_bytes+stats.nano_bytes},{"nodes",stats.node_counts},
                        {"tiles",{nano->tree().activeTileCount(1),nano->tree().activeTileCount(2),nano->tree().activeTileCount(3)}},{"active_voxels",stats.active_voxels},
                        {"aabb_as_sbt",nullptr},{"nano_checksum",hash_bytes(cpu.bytes())}};
                    double volume=1; for(int axis=0;axis<3;++axis) volume*=double(in.domain.maximum[axis])-in.domain.minimum[axis]+1;
                    row["storage"]["domain_voxels"]=volume; row["storage"]["occupancy"]=double(stats.active_voxels)/volume; raw.reset();
                    auto cache=take(VoxelGpuCache::create(q)); start=Clock::now(); auto grid=take(cache->prepare(q,cpu)); const auto submitted=Clock::now();
                    row["upload_pending"]=gpu_memory(q); require(take(q.wait(grid->upload_submission())),"upload incomplete");
                    row["upload"]={{"cpu_submit_ms",ms(start,submitted)},{"wait_ms",ms(submitted)},{"gpu",gpu_profile(grid->upload_submission())}};
                    auto oracle=dense(in); row["dense_reference_bytes"]=oracle ? oracle->values().size()*sizeof(std::uint32_t) : 0;
                    row["queries"]=queries(in,oracle,q,grid,tuned,baseline,scalar,samples,round);
                    row["renders"]=Json::array();
                    for(const auto extent:{std::array{640u,480u},std::array{1280u,720u}}) {
                        if(smoke && extent[0]!=640) continue;
                        for(bool ortho:{false,true}) row["renders"].push_back(render_case(in,cpu,oracle,heap,q,pipeline,meshes,scene,grid,extent[0],extent[1],ortho,samples));
                    }
                    // A distinct owning NanoVDB copy forces a real replacement generation.
                    auto copy=take(vx::load_sparse_grid(cpu.bytes(),cpu.domain(),cpu.corner_to_local(),1));
                    auto old=take(tuned.trace(q,grid,rays(in))); auto next=take(cache->prepare(q,copy)); row["old_generation_and_upload"]=gpu_memory(q);
                    std::vector<VoxelGpuResult> old_hits; require(take(old.read(q,old_hits)) && take(q.wait(next->upload_submission())),"old generation invalidated");
                    grid.reset(); old={}; next.reset(); cache.reset(); check(q.poll());
                }
                row["before_case"]=before; row["after_release"]=gpu_memory(q);
                require(row["after_release"]["allocations"]==before["allocations"] && row["after_release"]["allocation_bytes"]==before["allocation_bytes"],"case resources did not retire");
                output["cases"].push_back(std::move(row)); save_json(path,output);
            }
        }
        check(q.poll()); output["after_release"]=gpu_memory(q); require(output["after_release"]["allocations"]==output["device_only"]["allocations"],"GPU allocation leak"); check(q.close());
    }
    output["heap_peak_bytes"]=heap.snapshot().peak_backing_bytes; output["heap_live_allocations"]=heap.snapshot().live_allocations;
    output["diagnostics"]={{"errors",diagnostics.errors.load()},{"warnings",diagnostics.warnings.load()}};
    require(!diagnostics.errors && !diagnostics.warnings && !heap.snapshot().live_allocations && memory.try_close().closed(),"validation or CPU lifetime failure");
}
}
int main(int argc,char** argv) {
    Json output={{"schema",1},{"kind","voxel-raster-baseline"},{"status","failed"}}; std::filesystem::path path;
    try {
        bool smoke=false,validation=false; unsigned samples=8,round=0;
        for(int i=1;i<argc;++i) { const std::string_view arg=argv[i];
            if(arg=="--smoke") smoke=true; else if(arg=="--validation") validation=true;
            else if(arg=="--output" && i+1<argc) path=argv[++i];
            else if(arg=="--round" && i+1<argc) { const std::string s=argv[++i]; require(s.size()==1 && s[0]>='0' && s[0]<='2',"round must be 0..2"); round=unsigned(s[0]-'0'); }
            else throw std::runtime_error("unknown or incomplete voxel benchmark argument");
        }
        require(!path.empty(),"--output required"); if(smoke) samples=2;
        std::filesystem::create_directories(path.parent_path());
        output.update({{"configuration",DK_BENCH_CONFIG},{"compiler",DK_BENCH_COMPILER},{"validation",validation},{"smoke",smoke},{"round",round},
            {"steady_samples",samples},{"warmup",2},{"matrix_version",1},{"measurement_version",2},{"device_precondition_frames",smoke ? 0 : 64},{"tracy_connected",profiling::is_connected()},
            {"shader_fingerprint",take(shader_fingerprint(DK_RENDER_SHADER_DIR,false,true))}});
        run(output,smoke,validation,samples,round,path);
        require(output["shader_fingerprint"]==take(shader_fingerprint(DK_RENDER_SHADER_DIR,false,true)),"shader changed during measurement"); output["status"]="passed";
    } catch(const std::exception& e) { output["error"]=e.what(); std::fprintf(stderr,"voxel benchmark: %s\n",e.what()); }
    if(!path.empty()) { try { save_json(path,output); } catch(...) { return 1; } }
    return output["status"]=="passed" ? 0 : 1;
}

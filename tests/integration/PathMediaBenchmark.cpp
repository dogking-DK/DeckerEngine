#include "PathBenchmarkSupport.hpp"
#include "PathPreview.hpp"
#include <dk/editor/Workspace.hpp>
#include <thread>
using namespace dk::editor;
namespace {
struct Case { const char* name; const char* preset; Vec3d eye,target; bool surface=false,low=false,fog=false; };
const std::array cases={
    Case{"surface","clear",{8,1.8,0},{-4,2,0},true},
    Case{"courtyard-clear","clear",{8,1.8,0},{-4,2,0}},
    Case{"courtyard-sparse","sparse",{8,1.8,0},{-4,2,0}},
    Case{"sky-clear","clear",{8,1.8,0},{8,5,1}},
    Case{"sky-sparse","sparse",{8,1.8,0},{8,5,1}},
    Case{"sky-overcast","overcast",{8,1.8,0},{8,5,1}},
    Case{"low-sun","sparse",{8,1.8,0},{-4,2,0},false,true},
    Case{"fog","clear",{8,1.8,0},{-4,2,0},false,false,true},
    Case{"cloud-inside","sparse",{8,2000,0},{8,2000.2,1}},
    Case{"cloud-above","sparse",{8,4500,0},{8,4500.2,1}}};
Transformd look(Vec3d eye,Vec3d target) {
    const Vec3d back=(eye-target).normalized(),right=Vec3d::UnitY().cross(back).normalized(),up=back.cross(right);
    Mat4d m=Mat4d::Identity(); m.block<3,1>(0,0)=right; m.block<3,1>(0,1)=up; m.block<3,1>(0,2)=back; m.block<3,1>(0,3)=eye;
    return take(Transformd::from_matrix(m));
}
RenderView view(const RenderScene& scene,const Case& c,unsigned w,unsigned h,double shift=0) {
    Camera projection; projection.set_environment(true);
    return take(RenderView::create(scene,{w,h,0,look(c.eye+Vec3d{shift,0,0},c.target+Vec3d{shift,0,0}),projection.projection(double(w)/h)}));
}
VolumePathSettings volume(unsigned seed=101) {
    VolumePathSettings s; s.seed=seed; s.max_bounces=4; s.rr_start=3; s.environment_rgb={.3,.4,.6}; s.sun=true;
    s.max_tracking_steps=1ull<<28; return s;
}
PathTraceSettings surface(unsigned seed=101) {
    PathTraceSettings s; s.seed=seed; s.max_bounces=4; s.rr_start=3; s.environment_rgb={.3,.4,.6}; s.sun=PathTraceSun::scene; s.surface_only=true; return s;
}
void set_environment(Workspace& model,const Case& c) {
    check(model.preset_environment(c.preset));
    auto& value=model.environment_draft()->value;
    if(c.low) value["sun"]["direction"]={1,.02,.1};
    if(c.fog) {
        value["atmosphere"]["mie_scattering_per_m"]={.00008,.00008,.00008};
        value["atmosphere"]["mie_extinction_per_m"]={.0001,.0001,.0001};
    }
    check(model.apply_environment());
}
PathMediumSnapshot medium(const EnvironmentDescription& e,CloudFieldCache& cache) {
    std::shared_ptr<const CloudFieldData> field;
    if(e.clouds.enabled && e.clouds.coverage>0 && e.clouds.density>0 && e.clouds.extinction_per_m>0) field=take(cache.prepare(e.clouds));
    return take(PathMediumSnapshot::create(e,{1,17,.9,.6},field));
}
void configure(PathTraceRenderer& r,SubmissionQueue& q,const PathTraceGpuScene& gpu,const PathMediumSnapshot& m,const RenderView& v,
    bool only,unsigned seed,const PathTraceSchedule& schedule) {
    if(only) check(r.configure_view(q,1,gpu,v,surface(seed),schedule));
    else check(r.configure_medium_view(q,1,gpu,m,v,volume(seed),schedule));
}
Json image(const PathTraceGpuImage& value) {
    return {{"linear",pixels(value.linear_rgb())},{"second",pixels(value.second_moment())},{"samples",value.info().samples.count}};
}
void cpu_quality(PathTraceRenderer& r,SubmissionQueue& q,memory::ResourceHandle heap,Json& out) {
    out=Json::array();
    for(const char* name:{"air-surface","cloud-surface","rough-reflection","near-mirror"}) {
        Fixture f{heap,std::string_view{name}.starts_with("air") || std::string_view{name}.starts_with("cloud") ? "area-lambert" : "ggx-box"};
        auto e=take(environment_preset(std::string_view{name}.starts_with("cloud") ? "sparse" : "clear"));
        e.atmosphere.planet_center_m[1]-=2;
        if(std::string_view{name}=="near-mirror") { f.asset.materials[0].metallic=1; f.asset.materials[0].roughness=.05; }
        check(f.document->set_environment(e)); f.source=take(RenderScene::extract(heap,take(f.document->snapshot())));
        const std::array assets{&f.asset}; f.cpu=take(PathTraceScene::create(heap,f.source,assets,{},f.model));
        auto gpu=take(r.prepare_scene(q,f.cpu)); CloudFieldCache fields; auto m=medium(e,fields);
        const PathTraceRay ray{{0,.25,1},Vec3d{.15,-.1,-1}.normalized(),.05,10000};
        Json item={{"name",name},{"seeds",Json::array()}};
        for(unsigned i=0;i<8;++i) {
            auto s=volume(101+7919*i); const auto start=Clock::now();
            auto n=take(estimate_volume_path_reference(f.cpu,m,ray,s,{0,16384}));
            auto four=take(estimate_volume_path_reference(f.cpu,m,ray,s,{0,65536}));
            auto actual=read_image(q,take(r.submit_medium_rays(q,gpu,m,std::span{&ray,1},s,{0,65536})));
            item["seeds"].push_back({{"seed",s.seed},{"cpu_n",{n.mean.x(),n.mean.y(),n.mean.z()}},
                {"cpu_4n",{four.mean.x(),four.mean.y(),four.mean.z()}},{"gpu",pixels(actual.linear_rgb())[0]},
                {"second",{four.second_moment.x(),four.second_moment.y(),four.second_moment.z()}},{"wall_ms",elapsed(start)}});
        }
        out.push_back(std::move(item)); std::printf("CPU quality %s complete\n",name); std::fflush(stdout);
    }
}
void run(const std::filesystem::path& root,const std::filesystem::path& shaders,Json& out,bool quality,bool smoke,unsigned edge) {
    auto model=take(Workspace::create(root)); check(model->open("project.json"));
    auto system=take(memory::MemorySystem::create()); auto heap=take(system.create_heap({"media-benchmark",memory::DomainCategory::render}));
    Diagnostics diagnostics;
    {
        memory::ThreadContext context{system,heap}; memory::ExecutionScope scope{context,heap};
        DeviceOptions options; options.ray_tracing=RayTracingMode::required; options.sampled_image_indexing=SampledImageIndexingMode::required;
        options.validation=quality || smoke ? ValidationMode::required : ValidationMode::disabled;
        options.diagnostic_sink=diagnostic; options.diagnostic_user_data=&diagnostics;
        auto device=take(Device::create(heap,options)); out["device"]=device.adapter().properties.deviceName.data(); out["driver"]=device.adapter().driver.driverInfo.data();
        auto q=take(SubmissionQueue::create(heap,std::move(device))); check(q.configure_gpu_profiling({!quality,64}));
        {
            auto start=Clock::now(); auto r=take(PathTraceRenderer::create(heap,q,shaders)); out["compile_ms"]=elapsed(start);
            if(quality) cpu_quality(r,q,heap,out["cpu_quality"]);
            start=Clock::now(); DiskSceneOptions disk_options; disk_options.profile=GltfImportProfile::unlit_preview;
            auto disk=take(DiskScene::load_snapshot(heap,model->snapshot()->project,model->snapshot()->scene,disk_options));
            out["asset_load_ms"]=elapsed(start); std::vector<const CpuAsset*> assets; for(const auto& a:disk.assets()) assets.push_back(a.get());
            CloudFieldCache fields; PathTraceGpuScene gpu; PathTraceScene previous_cpu;
            for(const auto& c:cases) {
                if(smoke && std::string_view{c.name}!="courtyard-sparse" && !c.surface) continue;
                set_environment(*model,c); start=Clock::now(); auto scene=take(RenderScene::extract(heap,model->snapshot()->scene));
                auto cpu=previous_cpu ? take(previous_cpu.rebind(heap,scene)) : take(PathTraceScene::create(heap,scene,assets,desktop_path_scene_limits(),PathTraceMaterialModel::metallic_roughness));
                previous_cpu=cpu;
                Json item={{"name",c.name},{"cpu_scene_ms",elapsed(start)},{"triangles",cpu.triangle_count()},{"geometry",cpu.geometry_fingerprint()},
                    {"environment",take(model->execute_console(R"({"method":"environment.query","params":{}})"))["environment"]},
                    {"eye",{c.eye.x(),c.eye.y(),c.eye.z()}},{"target",{c.target.x(),c.target.y(),c.target.z()}}};
                start=Clock::now(); gpu=take(r.prepare_scene(q,cpu)); item["gpu_prepare_ms"]=elapsed(start);
                auto m=medium(scene.environment(),fields); item["field_hash"]=m.field_hash();
                if(quality) {
                    item["seeds"]=Json::array(); const auto v=view(scene,c,4,4);
                    for(unsigned i=0;i<8;++i) {
                        Json row={{"seed",101+7919*i},{"reference_seed",104729+101+7919*i},{"levels",Json::array()}};
                        PathTraceSchedule schedule; schedule.batch_spp=16; schedule.tile_edge=edge;
                        for(unsigned target:{64u,256u,1024u,4096u,16384u}) {
                            schedule.target_spp=target; configure(r,q,gpu,m,v,c.surface,101+7919*i,schedule);
                            auto measured=advance(r,q,1,false); auto value=read_image(q,take(r.completed_frame(1).read_linear(q)));
                            row["levels"].push_back({{"spp",target},{"increment_ms",measured["wall_ms"]},{"image",image(value)}});
                        }
                        for(unsigned target:{16384u,65536u}) {
                            schedule.target_spp=target; configure(r,q,gpu,m,v,c.surface,104729+101+7919*i,schedule); advance(r,q,1,false);
                            row[target==16384 ? "reference_n" : "reference_4n"]=image(read_image(q,take(r.completed_frame(1).read_linear(q))));
                        }
                        item["seeds"].push_back(std::move(row));
                    }
                } else {
                    const unsigned w=smoke ? 96 : 640,h=smoke ? 54 : 360; const auto v=view(scene,c,w,h);
                    item["width"]=w; item["height"]=h; item["frames"]=Json::array();
                    for(unsigned target=1;target<=(smoke ? 2u : 4u);++target) {
                        PathTraceSchedule schedule; schedule.target_spp=target; schedule.tile_edge=edge;
                        configure(r,q,gpu,m,v,c.surface,101,schedule); auto frame=advance(r,q,1,true); frame["memory"]=memory(r,q);
                        item["frames"].push_back(std::move(frame));
                    }
                    // A fixed small image is retained for exact before/after sample comparison.
                    PathTraceSchedule schedule; schedule.target_spp=4; schedule.tile_edge=edge;
                    configure(r,q,gpu,m,view(scene,c,32,18),c.surface,101,schedule); advance(r,q,1,false);
                    item["comparison"]=image(read_image(q,take(r.completed_frame(1).read_linear(q))));
                    item["preview"]=Json::array(); item["cancel"]=Json::array();
                    for(unsigned i=0;i<(smoke ? 2u : 12u);++i) {
                        schedule.target_spp=1; const auto moved=view(scene,c,96,54,.01*(i+1)); start=Clock::now();
                        configure(r,q,gpu,m,moved,c.surface,101,schedule); advance(r,q,1,false);
                        auto display=take(r.display(q,r.completed_frame(1),0,true)); require(take(q.wait(display.submission)),"Preview display wait");
                        item["preview"].push_back(elapsed(start));
                        schedule.target_spp=64; configure(r,q,gpu,m,moved,c.surface,101,schedule); require(take(r.submit_batch(q,1)),"Cancellation tile missing");
                        auto ticket=r.pending_submission(); start=Clock::now(); check(r.cancel(1)); const double call=elapsed(start);
                        require(take(q.wait(ticket)),"Cancel drain wait"); (void)take(r.poll(q)); item["cancel"].push_back({{"call_ms",call},{"reclaim_ms",elapsed(start)}});
                    }
                    item["memory"]=memory(r,q);
                }
                check(r.release_view(1)); out["cases"].push_back(std::move(item)); std::printf("Sponza %s complete\n",c.name); std::fflush(stdout);
            }
        }
        check(q.close()); VmaTotalStatistics stats{}; vmaCalculateStatistics(q.device().allocator(),&stats); out["vma_live"]=stats.total.statistics.allocationCount;
        require(!stats.total.statistics.allocationCount,"GPU allocation leak");
    }
    out["cpu_live"]=heap.snapshot().live_allocations; out["cpu_peak"]=heap.snapshot().peak_backing_bytes;
    out["diagnostics"]={{"errors",diagnostics.errors.load()},{"warnings",diagnostics.warnings.load()}};
    require(!diagnostics.errors && !diagnostics.warnings && !heap.snapshot().live_allocations,"Diagnostics or CPU leak");
    require(system.try_close().closed(),"Memory close");
}
}
int main(int argc,char** argv) {
    Json output={{"schema",1},{"status","failed"},{"cases",Json::array()}}; std::filesystem::path report,root,shaders=DK_PATH_SHADER_DIR;
    try {
        bool quality=false,smoke=false; unsigned edge=64;
        for(int i=1;i<argc;++i) {
            std::string_view option=argv[i];
            if(option=="--quality") quality=true; else if(option=="--smoke") smoke=true;
            else if(option=="--root" && i+1<argc) root=argv[++i]; else if(option=="--output" && i+1<argc) report=argv[++i];
            else if(option=="--shaders" && i+1<argc) shaders=argv[++i]; else if(option=="--tile" && i+1<argc) edge=unsigned(std::stoul(argv[++i]));
            else throw std::runtime_error("Unknown media benchmark argument");
        }
        require(!root.empty() && !report.empty() && edge>0 && edge<=64,"Root/output/tile required"); std::filesystem::create_directories(report.parent_path());
        output.update({{"kind",quality ? "quality" : "performance"},{"validation",quality || smoke},{"smoke",smoke},{"tile",edge},
            {"model",4},{"sampler",2},{"time",17},{"bounces",4},{"configuration",DK_BENCH_CONFIG}});
        run(root,shaders,output,quality,smoke,edge); output["status"]="passed";
    } catch(const std::exception& e) { output["error"]=e.what(); std::fprintf(stderr,"Media benchmark: %s\n",e.what()); }
    if(!report.empty()) { std::ofstream file(report); file<<output.dump(2)<<'\n'; if(!file) return 1; }
    return output["status"]=="passed" ? 0 : 1;
}

#include "PathBenchmarkSupport.hpp"
#include "PathPreview.hpp"
#include <dk/editor/Workspace.hpp>
#include <thread>
using namespace dk::editor;
namespace {
// Optional acceptance controls; defaults preserve the M17.6 protocol.
std::string algorithm="classic", selected_case;
unsigned quality_width=4,quality_height=4,quality_limit=16384,reference_n=16384;
unsigned performance_samples=4,preview_width=96,preview_height=54;
std::filesystem::path linear_directory;
bool skip_reference=false,timing_quality=false,full_preview=false;
std::optional<RestirDiSettings> restir_config() {
    if(algorithm=="classic") return {};
    RestirDiSettings value;
    if(algorithm=="temporal" || algorithm=="spatial") value.temporal.emplace();
    if(algorithm=="spatial") value.spatial.emplace();
    return value;
}
Json restir_json(const std::optional<RestirDiSettings>& value) {
    if(!value) return nullptr;
    Json result={{"version",value->version},{"initial_candidates",value->initial_candidates},
        {"max_candidate_evaluations",value->max_candidate_evaluations},{"temporal",nullptr},{"spatial",nullptr}};
    if(value->temporal) { const auto& s=*value->temporal; result["temporal"]={{"version",s.version},
        {"max_history_candidates",s.max_history_candidates},{"max_history_age",s.max_history_age},
        {"position_tolerance",s.position_tolerance},{"normal_cosine",s.normal_cosine}}; }
    if(value->spatial) { const auto& s=*value->spatial; result["spatial"]={{"version",s.version},
        {"neighbor_count",s.neighbor_count},{"radius",s.radius},{"max_source_candidates",s.max_source_candidates},
        {"position_tolerance",s.position_tolerance},{"normal_cosine",s.normal_cosine}}; }
    return result;
}
void save_linear(const PathTraceGpuImage& value,const std::string& name,Json& row,bool save_file=true) {
    const auto& info=value.info();
    row["transport"]={{"medium",bool(info.medium)},{"model",info.medium ? info.medium->settings.model_version : info.settings.model_version},
        {"sampler",info.medium ? info.medium->settings.sampler_version : info.settings.sampler_version},
        {"seed",info.medium ? info.medium->settings.seed : info.settings.seed},
        {"max_bounces",info.medium ? info.medium->settings.max_bounces : info.settings.max_bounces},
        {"rr_start",info.medium ? info.medium->settings.rr_start : info.settings.rr_start},
        {"fingerprint",info.fingerprint},{"geometry_fingerprint",info.geometry_fingerprint}};
    row["source"]={{"sampling_first_epoch",info.sampling_first_epoch},{"empty_history",!info.temporal_history_epoch},
        {"restir",restir_json(info.restir)},{"samples",info.samples.count},
        {"history_epoch",info.temporal_history_epoch ? Json(*info.temporal_history_epoch) : Json(nullptr)},
        {"rays",info.statistics.rays},{"tracking_steps",nullptr},
        {"temporal_reused",info.temporal_statistics.reused},{"temporal_rejected",info.temporal_statistics.rejected},
        {"temporal_expired",info.temporal_statistics.expired},{"temporal_support_corrected",info.temporal_statistics.support_corrected},
        {"temporal_max_age",info.temporal_statistics.max_age},{"temporal_max_candidates",info.temporal_statistics.max_candidates},
        {"spatial_reused",info.spatial_statistics.reused},{"support_corrected",info.spatial_statistics.support_corrected},
        {"spatial_max_age",info.spatial_statistics.max_age},{"spatial_max_candidates",info.spatial_statistics.max_candidates},
        {"current_visibility_rays",info.spatial_statistics.current_visibility_rays},
        {"prepared_surfaces",info.restir_medium_statistics.prepared_surfaces},{"applied_surfaces",info.restir_medium_statistics.applied_surfaces}};
    if(linear_directory.empty() || !save_file) return;
    std::filesystem::create_directories(linear_directory);
    const auto path=linear_directory/(name+".rgb32f"); std::ofstream file(path,std::ios::binary);
    for(const auto& pixel:value.linear_rgb()) {
        require(pixel.allFinite(),"Nonfinite full image");
        const std::array<float,3> rgb{float(pixel.x()),float(pixel.y()),float(pixel.z())};
        file.write(reinterpret_cast<const char*>(rgb.data()),sizeof(rgb));
    }
    file.close(); require(bool(file),"Cannot save linear image");
    row["linear_file"]=path.generic_string(); row["linear_extent"]={info.width,info.height};
}
// Keep long-chain timing bounded: accumulate intervals, never retain millions of tile JSON records.
Json advance_quality(PathTraceRenderer& r,SubmissionQueue& q) {
    const auto start=Clock::now(); double gpu_ms=0; std::uint64_t submissions=0;
    while(take(r.status(1)).completed_spp<take(r.status(1)).target_spp) {
        require(take(r.submit_batch(q,1)),"Quality tile missing");
        const auto ticket=r.pending_submission();
        // Exclusive tiny-image benchmark only: bound active polling, then block on slow work.
        // The renderer still publishes through its normal completed-ticket checks.
        const auto deadline=Clock::now()+std::chrono::milliseconds(1);
        do { check(q.poll()); } while(q.stats().completed<ticket.value() && Clock::now()<deadline);
        require(take(q.wait(ticket)),"Quality tile wait"); (void)take(r.poll(q));
        if(timing_quality) { const auto p=ticket.gpu_profile();
            require(p.status()==GpuProfileStatus::ready && !p.dropped_zones(),"Missing quality GPU timestamps");
            unsigned count=0; for(const auto& t:p.timings()) if(t.kind==GpuZoneKind::submission) {
                require(std::isfinite(t.elapsed_ns) && t.elapsed_ns>=0,"Invalid quality timestamp");
                gpu_ms+=t.elapsed_ns/1e6; ++count;
            }
            require(count==1,"Missing quality total interval");
        }
        ++submissions;
    }
    return {{"wall_ms",elapsed(start)},{"gpu_ms",timing_quality ? Json(gpu_ms) : Json(nullptr)},{"submissions",submissions}};
}
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
    const auto restir=restir_config();
    if(restir) {
        if(only) check(r.configure_restir_view(q,1,gpu,v,surface(seed),*restir,schedule));
        else check(r.configure_restir_medium_view(q,1,gpu,m,v,volume(seed),*restir,schedule));
    } else if(only) check(r.configure_view(q,1,gpu,v,surface(seed),schedule));
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
        options.validation=(quality && !timing_quality) || smoke ? ValidationMode::required : ValidationMode::disabled;
        options.diagnostic_sink=diagnostic; options.diagnostic_user_data=&diagnostics;
        auto device=take(Device::create(heap,options)); out["device"]=device.adapter().properties.deviceName.data(); out["driver"]=device.adapter().driver.driverInfo.data();
        auto q=take(SubmissionQueue::create(heap,std::move(device))); check(q.configure_gpu_profiling({!quality || timing_quality,64}));
        {
            auto start=Clock::now(); auto r=take(PathTraceRenderer::create(heap,q,shaders)); out["compile_ms"]=elapsed(start);
            if(quality && !timing_quality && selected_case.empty() && algorithm=="classic") cpu_quality(r,q,heap,out["cpu_quality"]);
            start=Clock::now(); DiskSceneOptions disk_options; disk_options.profile=GltfImportProfile::unlit_preview;
            auto disk=take(DiskScene::load_snapshot(heap,model->snapshot()->project,model->snapshot()->scene,disk_options));
            out["asset_load_ms"]=elapsed(start); std::vector<const CpuAsset*> assets; for(const auto& a:disk.assets()) assets.push_back(a.get());
            CloudFieldCache fields; PathTraceGpuScene gpu; PathTraceScene previous_cpu;
            for(const auto& c:cases) {
                // Keep scene revision/fingerprint identical for a selected case and the complete suite.
                set_environment(*model,c);
                if(!selected_case.empty() && selected_case!=c.name) continue;
                if(smoke && std::string_view{c.name}!="courtyard-sparse" && !c.surface) continue;
                start=Clock::now(); auto scene=take(RenderScene::extract(heap,model->snapshot()->scene));
                auto cpu=previous_cpu ? take(previous_cpu.rebind(heap,scene)) : take(PathTraceScene::create(heap,scene,assets,desktop_path_scene_limits(),PathTraceMaterialModel::metallic_roughness));
                previous_cpu=cpu;
                Json item={{"name",c.name},{"cpu_scene_ms",elapsed(start)},{"triangles",cpu.triangle_count()},{"geometry",cpu.geometry_fingerprint()},
                    {"environment",take(model->execute_console(R"({"method":"environment.query","params":{}})"))["environment"]},
                    {"surface_only",c.surface},{"textures",cpu.texture_count()},{"fingerprint",cpu.fingerprint()},
                    {"eye",{c.eye.x(),c.eye.y(),c.eye.z()}},{"target",{c.target.x(),c.target.y(),c.target.z()}}};
                start=Clock::now(); gpu=take(r.prepare_scene(q,cpu)); item["gpu_prepare_ms"]=elapsed(start);
                auto m=medium(scene.environment(),fields); item["field_hash"]=m.field_hash();
                if(quality) {
                    item["seeds"]=Json::array(); const auto v=view(scene,c,quality_width,quality_height);
                    item["width"]=quality_width; item["height"]=quality_height;
                    for(unsigned i=0;i<8;++i) {
                        Json row={{"seed",101+7919*i},{"reference_seed",104729+101+7919*i},{"levels",Json::array()}};
                        PathTraceSchedule schedule; schedule.batch_spp=16; schedule.tile_edge=edge;
                        for(unsigned target:{64u,256u,1024u,4096u,16384u}) {
                            if(target>quality_limit) break;
                            schedule.target_spp=target; configure(r,q,gpu,m,v,c.surface,101+7919*i,schedule);
                            auto measured=advance_quality(r,q); auto value=read_image(q,take(r.completed_frame(1).read_linear(q)));
                            Json level={{"spp",target},{"increment_ms",measured["wall_ms"]},{"increment_gpu_ms",measured["gpu_ms"]},
                                {"submissions",measured["submissions"]},{"image",image(value)}};
                            save_linear(value,std::string{c.name}+"-"+algorithm+"-seed"+std::to_string(i)+"-spp"+std::to_string(target),level);
                            row["levels"].push_back(std::move(level));
                        }
                        // Independent classic reference, never a reused candidate chain.
                        const auto saved_algorithm=algorithm; algorithm="classic";
                        for(unsigned target:skip_reference ? std::vector<unsigned>{} : std::vector<unsigned>{reference_n,4*reference_n}) {
                            schedule.target_spp=target; configure(r,q,gpu,m,v,c.surface,104729+101+7919*i,schedule); advance_quality(r,q);
                            auto value=read_image(q,take(r.completed_frame(1).read_linear(q)));
                            auto& reference=row[target==reference_n ? "reference_n" : "reference_4n"]; reference=image(value);
                            save_linear(value,std::string{c.name}+"-reference-seed"+std::to_string(i)+"-spp"+std::to_string(target),reference);
                        }
                        algorithm=saved_algorithm;
                        check(r.release_view(1));
                        std::printf("quality %s %s seed %u complete\n",c.name,algorithm.c_str(),i); std::fflush(stdout);
                        item["seeds"].push_back(std::move(row));
                    }
                } else {
                    const unsigned w=smoke ? 96 : 640,h=smoke ? 54 : 360; const auto v=view(scene,c,w,h);
                    item["width"]=w; item["height"]=h; item["frames"]=Json::array();
                    for(unsigned target=1;target<=(smoke ? 2u : performance_samples);++target) {
                        PathTraceSchedule schedule; schedule.target_spp=target; schedule.tile_edge=edge;
                        configure(r,q,gpu,m,v,c.surface,101,schedule); auto frame=advance(r,q,1,true); frame["memory"]=memory(r,q);
                        save_linear(read_image(q,take(r.completed_frame(1).read_linear(q))),
                            std::string{c.name}+"-"+algorithm+"-spp"+std::to_string(target),frame,target==(smoke ? 2u : performance_samples));
                        item["frames"].push_back(std::move(frame));
                    }
                    // A fixed small image is retained for exact before/after sample comparison.
                    PathTraceSchedule schedule; schedule.target_spp=4; schedule.tile_edge=edge;
                    configure(r,q,gpu,m,view(scene,c,32,18),c.surface,101,schedule); advance(r,q,1,false);
                    item["comparison"]=image(read_image(q,take(r.completed_frame(1).read_linear(q))));
                    item["preview"]=Json::array(); item["cancel"]=Json::array(); item["motion"]=Json::array();
                    for(unsigned i=0;i<(smoke ? 2u : 12u);++i) {
                        schedule.target_spp=1; const auto moved=view(scene,c,preview_width,preview_height,.01*(i+1)); start=Clock::now();
                        configure(r,q,gpu,m,moved,c.surface,101,schedule); advance(r,q,1,false);
                        auto display=take(r.display(q,r.completed_frame(1),0,true)); require(take(q.wait(display.submission)),"Preview display wait");
                        item["preview"].push_back(elapsed(start));
                        if(full_preview) {
                            Json motion={{"shift",.01*(i+1)}};
                            save_linear(read_image(q,take(r.completed_frame(1).read_linear(q))),
                                std::string{c.name}+"-"+algorithm+"-moved"+std::to_string(i),motion,i==(smoke ? 1u : 11u));
                            item["motion"].push_back(std::move(motion));
                        }
                        schedule.target_spp=64; configure(r,q,gpu,m,moved,c.surface,101,schedule); require(take(r.submit_batch(q,1)),"Cancellation tile missing");
                        auto ticket=r.pending_submission(); start=Clock::now(); check(r.cancel(1)); const double call=elapsed(start);
                        require(take(q.wait(ticket)),"Cancel drain wait"); (void)take(r.poll(q)); item["cancel"].push_back({{"call_ms",call},{"reclaim_ms",elapsed(start)}});
                    }
                    if(full_preview) {
                        schedule.target_spp=16;
                        configure(r,q,gpu,m,view(scene,c,preview_width,preview_height,smoke ? .02 : .12),c.surface,101,schedule);
                        item["recovery"]=advance(r,q,1,false);
                        save_linear(read_image(q,take(r.completed_frame(1).read_linear(q))),
                            std::string{c.name}+"-"+algorithm+"-stopped16",item["recovery"]);
                        require(item["recovery"]["completed_spp"]==16,"Camera stop did not accumulate");
                    }
                    item["memory"]=memory(r,q);
                }
                if(!quality) check(r.release_view(1)); // Quality releases each independent chain above.
                out["cases"].push_back(std::move(item)); std::printf("Sponza %s complete\n",c.name); std::fflush(stdout);
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
            else if(option=="--timing-quality") { quality=true; timing_quality=true; }
            else if(option=="--root" && i+1<argc) root=argv[++i]; else if(option=="--output" && i+1<argc) report=argv[++i];
            else if(option=="--shaders" && i+1<argc) shaders=argv[++i]; else if(option=="--tile" && i+1<argc) edge=unsigned(std::stoul(argv[++i]));
            else if(option=="--algorithm" && i+1<argc) algorithm=argv[++i];
            else if(option=="--case" && i+1<argc) selected_case=argv[++i];
            else if(option=="--quality-width" && i+1<argc) quality_width=unsigned(std::stoul(argv[++i]));
            else if(option=="--quality-height" && i+1<argc) quality_height=unsigned(std::stoul(argv[++i]));
            else if(option=="--quality-limit" && i+1<argc) quality_limit=unsigned(std::stoul(argv[++i]));
            else if(option=="--reference-n" && i+1<argc) reference_n=unsigned(std::stoul(argv[++i]));
            else if(option=="--performance-samples" && i+1<argc) performance_samples=unsigned(std::stoul(argv[++i]));
            else if(option=="--skip-reference") skip_reference=true;
            else if(option=="--full-preview") { preview_width=640; preview_height=360; full_preview=true; }
            else if(option=="--linear-directory" && i+1<argc) linear_directory=argv[++i];
            else throw std::runtime_error("Unknown media benchmark argument");
        }
        require(algorithm=="classic" || algorithm=="initial" || algorithm=="temporal" || algorithm=="spatial","Unknown algorithm");
        require(selected_case.empty() || std::any_of(cases.begin(),cases.end(),[](const auto& c){return selected_case==c.name;}),"Unknown case");
        require(quality_width>0 && quality_width<=640 && quality_height>0 && quality_height<=360 &&
            quality_limit>=64 && quality_limit<=16384 && reference_n>=64 && reference_n<=16384 && performance_samples>0 && performance_samples<=16384,"Invalid benchmark bounds");
        output["algorithm"]=algorithm; output["rtxdi"]="3.1.0/f12037fa8e97ebc08e9e3edfd2de528ed1772a4b";
        output["quality_extent"]={quality_width,quality_height}; output["quality_limit"]=quality_limit;
        output["reference_n"]=reference_n; output["preview_extent"]={preview_width,preview_height};
        output["initial_candidates"]=algorithm=="classic" ? 0 : 8;
        output["restir"]=restir_json(restir_config()); output["timing_quality"]=timing_quality;
        output["quality_wait"]="active_poll_1ms_then_block";
        output["skip_reference"]=skip_reference; output["performance_samples"]=performance_samples;
        output["reconstruction"]="none"; output["tracking_observation"]="unavailable in GPU image diagnostics";
        require(!root.empty() && !report.empty() && edge>0 && edge<=256,"Root/output/tile required"); std::filesystem::create_directories(report.parent_path());
        output.update({{"kind",quality ? "quality" : "performance"},{"validation",(quality && !timing_quality) || smoke},{"smoke",smoke},{"tile",edge},
            {"model",4},{"sampler",2},{"time",17},{"bounces",4},{"configuration",DK_BENCH_CONFIG}});
        run(root,shaders,output,quality,smoke,edge); output["status"]="passed";
    } catch(const std::exception& e) { output["error"]=e.what(); std::fprintf(stderr,"Media benchmark: %s\n",e.what()); }
    if(!report.empty()) { std::ofstream file(report); file<<output.dump(2)<<'\n'; if(!file) return 1; }
    return output["status"]=="passed" ? 0 : 1;
}

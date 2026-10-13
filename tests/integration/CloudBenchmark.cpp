#include "CloudBenchmarkSupport.hpp"

namespace {
using Pixel=std::array<float,4>;
struct Fixture {
    memory::ResourceHandle heap;
    std::unique_ptr<SceneDocument> scene=take(SceneDocument::create());
    GpuAssets assets;
    ScenePipeline pipeline;
    AtmosphereRenderer atmosphere;
    CloudShadowRenderer shadows;
    CloudFieldCache fields;
    std::unique_ptr<CloudTextureCache> textures;
    std::shared_ptr<const CloudTextures> texture;
    EnvironmentDescription environment;
    AtmosphereSky sky;
    CloudShadowMap shadow;
    CloudOptimizedSettings optimized;
    RenderSettings settings=unlit_preview_settings();
    unsigned width=1920,height=1080;
    Vec3d eye{0,100,0};
    double angle=.16,yaw=0,time=0;
    Fixture(memory::ResourceHandle h,SubmissionQueue& queue,std::string_view preset):heap(h),
        assets(take(GpuAssets::create(h))),pipeline(take(ScenePipeline::create(h,queue,DK_RENDER_SHADER_DIR))),
        atmosphere(take(AtmosphereRenderer::create(h,queue,DK_ATMOSPHERE_SHADER_DIR))),
        shadows(take(CloudShadowRenderer::create(h,queue,DK_CLOUD_SHADER_DIR))),
        textures(take(CloudTextureCache::create(queue))),environment(take(environment_preset(preset))) {
        settings.exposure=8;
        CpuAsset asset; asset.mesh.id=take(AssetId::generate());
        TextureData tex; tex.id=take(AssetId::generate()); tex.width=2; tex.height=2;
        tex.rgba8={std::byte{255},std::byte{255},std::byte{255},std::byte{0},std::byte{255},std::byte{255},std::byte{255},std::byte{255},
                   std::byte{255},std::byte{255},std::byte{255},std::byte{255},std::byte{255},std::byte{255},std::byte{255},std::byte{0}};
        tex.sampler.min_filter=tex.sampler.mag_filter=TextureFilter::nearest; asset.textures.push_back(tex);
        for(unsigned index=0;index<2;++index) {
            MeshPrimitive p; p.positions={{-.14f,-.06f,0},{.14f,-.06f,0},{.14f,.06f,0},{-.14f,.06f,0}};
            p.normals.assign(4,Vec3f{0,0,1}); p.indices={0,1,2,0,2,3}; p.texcoords={{0,0},{1,0},{1,1},{0,1}};
            MaterialData m; m.id=take(AssetId::generate()); m.base_color={.8f,.3f,.1f,1}; m.double_sided=true;
            if(index) { m.alpha_mode=AlphaMode::mask; m.base_color_texture=tex.id; }
            p.material=m.id;
            const double distance=index ? 3000. : 500.,offset=index ? .38 : -.38;
            const auto rotation=camera_rotation();
            for(auto& v:p.positions) v=(eye+rotation*Vec3d{(v.x()+offset)*distance,v.y()*distance,-distance}).cast<float>();
            for(auto& v:p.normals) v=(rotation*v.cast<double>()).cast<float>();
            asset.materials.push_back(m); asset.mesh.primitives.push_back(p);
        }
        const auto entity=take(EntityId::generate()); check(scene->create_entity(entity));
        check(scene->set_asset_references(entity,{{asset.mesh.id,AssetKind::mesh}}));
        auto upload=take(assets.upload(queue,asset)); require(take(upload.wait(queue)),"geometry upload failed");
    }
    Quatd camera_rotation() const {
        const Vec3d forward{std::cos(angle)*std::cos(yaw),std::sin(angle),std::cos(angle)*std::sin(yaw)};
        const Vec3d right=forward.cross(Vec3d::UnitY()).normalized(),up=right.cross(forward);
        Mat3d basis; basis.col(0)=right; basis.col(1)=up; basis.col(2)=-forward; return Quatd{basis};
    }
    RenderView view() {
        ViewDescription d{width,height}; Trsd camera; camera.translation=eye; camera.rotation=camera_rotation();
        d.camera_world=take(Transformd::from_trs(camera)); d.projection=Mat4d::Zero();
        const double y=1/std::tan(1.15/2); d.projection(0,0)=y*height/width; d.projection(1,1)=-y;
        d.projection(2,2)=10000./(1-10000); d.projection(2,3)=10000./(1-10000); d.projection(3,2)=-1;
        return take(RenderView::create(take(RenderScene::extract(heap,take(scene->snapshot()))),d));
    }
    void reset(SubmissionQueue& q) {
        check(pipeline.poll_cloud_history(q));
        if(pipeline.cloud_history(1)) check(pipeline.release_cloud_history(q,1));
    }
    void prepare(SubmissionQueue& q,const EnvironmentDescription& e) {
        if(e.enabled) sky=take(atmosphere.prepare(q,e,eye));
        if(e.clouds.enabled) {
            CloudShadowSettings s; s.time_seconds=time;
            shadow=take(shadows.prepare(q,e,*texture,s));
        } else shadow={};
        optimized.composition.reference.time_seconds=time;
    }
    RenderFrame render(SubmissionQueue& q,const RenderView& v,const EnvironmentDescription& e) {
        if(e.clouds.enabled) return take(pipeline.render_clouds_optimized(q,v,assets,sky,*texture,optimized,settings,&shadow));
        if(e.enabled) return take(pipeline.render_atmosphere(q,v,assets,sky,settings));
        return take(pipeline.render(q,v,assets,settings));
    }
    Json input(std::string_view preset) const {
        return {{"name",preset},{"environment",environment_json(environment)},{"width",width},{"height",height},
            {"eye_m",{0,100,0}},{"elevation_rad",.16},{"yaw_rad",0},{"vertical_fov_rad",1.15},{"near_m",1},{"far_m",10000},{"exposure",8},
            {"geometry_distances_m",{500,3000}},{"second_receiver","checker alpha mask"},{"time_seconds",0},
            {"motion_step_m",.25},{"motion_yaw_step_rad",.0005},{"motion_time_step_seconds",1./30},
            {"quality","cloud_temporal_v1"},{"warmup",32},{"shadow",{{"center_m",{0,0,0}},{"extent_m",8192},{"resolution",256},{"steps",256}}}};
    }
};
std::vector<Pixel> linear(const RenderFrame& frame) {
    std::vector<Pixel> out(std::size_t{frame.info().width}*frame.info().height); check(frame.read_linear_rgba32f(out)); return out;
}
void image(const RenderFrame& frame,const std::filesystem::path& path) {
    std::vector<std::byte> p(std::size_t{frame.info().width}*frame.info().height*4); check(frame.read_rgba8(p));
    ppm(path,p,frame.info().width,frame.info().height);
}
void performance(Fixture& f,SubmissionQueue& q,Json& output,bool timestamps,unsigned samples,const std::filesystem::path& report) {
    output["preset"]=f.input(output["preset_name"].get<std::string>());
    output["frames"]=Json::array(); output["memory_samples"]=Json::array();
    for(const std::string mode:{"combined-static","combined-motion","air-static","air-motion","unlit-static","unlit-motion"}) {
        const bool combined=mode.starts_with("combined"),air=!mode.starts_with("unlit"),motion=mode.ends_with("motion");
        auto e=f.environment; e.enabled=air; e.clouds.enabled=combined && f.environment.clouds.enabled;
        check(f.scene->set_environment(e)); f.reset(q);
        const unsigned warmup=combined ? 32 : 5,count=1+warmup+samples;
        auto draw=[&](unsigned i,bool measure) {
            // Same steady index gives identical physical input in every baseline.
            const int logical=int(i)-int(warmup)-1;
            const double step=motion ? std::max(0,logical+1) : 0;
            f.eye={step*.25,100,0}; f.yaw=step*.0005; f.time=step/30;
            const auto start=Clock::now(); auto v=f.view(); const auto extracted=Clock::now();
            const auto old=generations(f.atmosphere.generations()); const auto old_shadow=f.shadows.current().generation();
            f.prepare(q,e); const auto prepared=Clock::now(); auto frame=f.render(q,v,e); const auto submitted=Clock::now();
            if(!measure) return frame;
            require(take(frame.wait(q)),"benchmark wait failed"); const auto waited=Clock::now();
            std::vector<std::byte> pixels(std::size_t{f.width}*f.height*4); check(frame.read_rgba8(pixels)); const auto done=Clock::now();
            Json luts=Json::object(); const auto after=generations(f.atmosphere.generations());
            for(unsigned k=0;k<3;++k) if(air && old[k]!=after[k])
                luts[std::array{"transmittance","multiple_scattering","sky_view"}[k]]=profile(take(f.sky.lut_profile(static_cast<AtmosphereLut>(k))),timestamps);
            Json shadow_profile=nullptr; const bool shadow_update=e.clouds.enabled && old_shadow!=f.shadow.generation();
            if(shadow_update) shadow_profile=profile(f.shadow.submission().gpu_profile(),timestamps);
            const auto history=frame.cloud_optimization();
            output["frames"].push_back({{"mode",mode},{"index",i},{"phase",i==0 ? "cold" : i<=warmup ? "warmup" : "steady"},
                {"logical_index",logical},{"eye_m",{f.eye.x(),f.eye.y(),f.eye.z()}},{"yaw_rad",f.yaw},{"time_seconds",f.time},
                {"extract_ms",elapsed(start,extracted)},{"prepare_ms",elapsed(extracted,prepared)},
                {"record_submit_ms",elapsed(prepared,submitted)},{"wait_ms",elapsed(submitted,waited)},
                {"readback_ms",elapsed(waited,done)},{"end_to_end_ms",elapsed(start,done)},
                {"generations",after},{"lut_builds",std::move(luts)},{"shadow_updated",shadow_update},{"shadow_gpu",std::move(shadow_profile)},
                {"shadow_generation",frame.cloud_shadow_generation()},
                {"history",{{"available",history.history_available},{"reset",cloud_history_reset_name(history.reset)},{"source",history.source_sequence},{"candidate",history.candidate_sequence}}},
                {"gpu",profile(frame.submission().gpu_profile(),timestamps)}});
            if(mode=="combined-static" && i==count-1) ppm(report.parent_path()/(report.stem().string()+".ppm"),pixels,f.width,f.height);
            return frame;
        };
        for(unsigned i=0;i<count;++i) { auto frame=draw(i,true); }
        // Untimed diagnostic with both an external completed Frame and the next candidate alive.
        auto prior=draw(count,false); require(take(prior.wait(q)),"prior memory wait failed");
        auto pending=draw(count+1,false); const auto inflight=memory_sample(q);
        require(take(pending.wait(q)),"inflight memory wait failed"); prior={}; pending={}; check(f.pipeline.poll_cloud_history(q));
        output["memory_samples"].push_back({{"mode",mode},{"inflight_with_old_frame",inflight},{"steady_cache_history",memory_sample(q)}});
    }
}
#include "CloudQualityCases.hpp"
void run(Json& output,std::string_view preset,bool smoke,bool quality,bool timestamps,bool validation,const std::filesystem::path& report) {
    auto system=take(memory::MemorySystem::create()); auto heap=take(system.create_heap({"cloud-benchmark",memory::DomainCategory::render}));
    Diagnostics diagnostics;
    {
        memory::ThreadContext thread{system}; memory::ExecutionScope scope{thread,heap};
        DeviceOptions options; options.validation=validation ? ValidationMode::required : ValidationMode::disabled;
        options.diagnostic_sink=diagnostic; options.diagnostic_user_data=&diagnostics;
        auto start=Clock::now(); auto q=take(SubmissionQueue::create(heap,take(Device::create(heap,options)),2));
        output["device_queue_ms"]=elapsed(start); check(q.configure_gpu_profiling({timestamps}));
        const auto& adapter=q.device().adapter(); output["device"]={{"name",adapter.properties.deviceName.data()},{"driver",adapter.driver.driverInfo.data()}};
        output["memory_device_only"]=memory_sample(q);
        {
            start=Clock::now(); Fixture f(heap,q,preset); output["program_geometry_create_ms"]=elapsed(start);
            output["memory_geometry_only"]=memory_sample(q);
            start=Clock::now(); const auto data=take(f.fields.prepare(f.environment.clouds)); output["cpu_field_ms"]=elapsed(start);
            start=Clock::now(); f.texture=take(f.textures->prepare(q,*data)); output["texture_upload_wall_ms"]=elapsed(start);
            output["field"]={{"hash",data->hash()},{"bytes",data->bytes()},{"gpu_upload_ms",nullptr}};
            if(quality) quality_cases(f,q,output,report);
            else { if(smoke) { f.width=160; f.height=90; } performance(f,q,output,timestamps,smoke ? 2 : 20,report); }
        }
        check(q.poll()); output["memory_after_release"]=memory_sample(q);
        require(output["memory_after_release"]["all_allocations"]==output["memory_device_only"]["all_allocations"],"GPU resources leaked"); check(q.close());
    }
    output["cpu_heap_peak_bytes"]=heap.snapshot().peak_backing_bytes;
    output["cpu_heap_live_allocations"]=heap.snapshot().live_allocations;
    output["diagnostics"]={{"errors",diagnostics.errors.load()},{"warnings",diagnostics.warnings.load()}};
    require(!diagnostics.errors && !diagnostics.warnings && !heap.snapshot().live_allocations,"diagnostic or CPU lifetime failure");
    require(system.try_close().closed(),"memory close failed");
}
}
int main(int argc,char** argv) {
    Json output={{"schema",1},{"status","failed"}}; std::filesystem::path report;
    try {
        std::string preset="sparse"; bool timestamps=false,validation=false,smoke=false,quality=false;
        for(int i=1;i<argc;++i) {
            const std::string_view arg=argv[i];
            if(arg=="--timestamps") timestamps=true;
            else if(arg=="--validation") validation=true;
            else if(arg=="--smoke") smoke=true;
            else if(arg=="--quality") quality=true;
            else if(arg=="--preset" && i+1<argc) preset=argv[++i];
            else if(arg=="--output" && i+1<argc) report=argv[++i];
            else throw std::runtime_error("unknown or incomplete cloud benchmark option");
        }
        require(!report.empty() && !(smoke && quality),"output and one run mode required");
        take(environment_preset(preset)); require(!quality || validation,"quality requires validation");
        if(!report.parent_path().empty()) std::filesystem::create_directories(report.parent_path());
        output.update({{"preset_name",preset},{"kind",quality ? "quality" : "performance"},{"compiler",DK_BENCH_COMPILER},{"configuration",DK_BENCH_CONFIG},
            {"timestamps",timestamps},{"validation",validation},{"smoke",smoke},{"steady_samples",smoke ? 2 : 20},
            {"tracy_compiled",profiling::enabled()},{"tracy_connected",profiling::is_connected()},
            {"shader_fingerprint",take(shader_fingerprint(DK_RENDER_SHADER_DIR,true))}});
        run(output,preset,smoke,quality,timestamps,validation,report);
        require(output["shader_fingerprint"]==take(shader_fingerprint(DK_RENDER_SHADER_DIR,true)),"shader changed during run"); output["status"]="passed";
    } catch(const std::exception& e) { output["error"]=e.what(); std::fprintf(stderr,"cloud benchmark: %s\n",e.what()); }
    if(!report.empty()) { std::ofstream stream(report); stream<<output.dump(2)<<'\n'; if(!stream) return 1; }
    return output["status"]=="passed" ? 0 : 1;
}

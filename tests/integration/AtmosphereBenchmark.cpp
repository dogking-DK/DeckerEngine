#include "../RenderAssetSupport.hpp"
#include <dk/render/Atmosphere.hpp>
#include <dk/render/ScenePipeline.hpp>
#include <dk/scene/SceneDocument.hpp>
#include <dk/profiling/Profiler.hpp>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <numbers>

using namespace dk;
using namespace dk::render;
using namespace dk::graphics;
using Json=nlohmann::json;
using Clock=std::chrono::steady_clock;
namespace {
void require(bool b,const char* message) { if(!b) throw std::runtime_error(message); }
template<class T,class E> T take(std::expected<T,E> r) {
    if(!r) { if constexpr(std::is_same_v<E,Error>) throw std::runtime_error(r.error().message); else throw std::bad_alloc{}; }
    return std::move(*r);
}
void check(Result<void> r) { if(!r) throw std::runtime_error(r.error().message); }
double elapsed(Clock::time_point a,Clock::time_point b=Clock::now()) { return std::chrono::duration<double,std::milli>(b-a).count(); }
struct Diagnostics { std::atomic<unsigned> errors=0,warnings=0; };
void diagnostic(void* user,const Diagnostic& d) noexcept {
    auto& counts=*static_cast<Diagnostics*>(user);
    if(d.severity==VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) ++counts.errors;
    if(d.severity==VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) ++counts.warnings;
    if(d.severity>=VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) std::fprintf(stderr,"%.*s\n",int(d.message.size()),d.message.data());
}
Json profile(const GpuProfile& p,bool enabled) {
    if(!enabled) { require(p.status()==GpuProfileStatus::disabled,"unexpected acquisition"); return nullptr; }
    require(p.status()==GpuProfileStatus::ready && p.dropped_zones()==0 && p.timestamp_valid_bits()>0 && p.timestamp_period_ns()>0,"missing GPU profile");
    Json result={{"submission_id",p.submission_value()},{"period_ns",p.timestamp_period_ns()},
        {"valid_bits",p.timestamp_valid_bits()},{"alignment_window_ns",p.alignment_window_ns()},
        {"submission_ms",0.0},{"zones",Json::array()}};
    unsigned submissions=0;
    for(const auto& t:p.timings()) {
        require(std::isfinite(t.elapsed_ns) && t.elapsed_ns>=0,"invalid GPU interval");
        result["zones"].push_back({{"name",t.name},{"gpu_ms",t.elapsed_ns/1e6},{"cpu_record_ms",double(t.cpu_end_ns-t.cpu_begin_ns)/1e6}});
        if(t.kind==GpuZoneKind::submission) { result["submission_ms"]=t.elapsed_ns/1e6; ++submissions; }
    }
    require(submissions==1,"missing submission interval"); return result;
}
Json memory_sample(const SubmissionQueue& queue) {
    VmaTotalStatistics stats{}; vmaCalculateStatistics(queue.device().allocator(),&stats);
    const VkPhysicalDeviceMemoryProperties* properties=nullptr; vmaGetMemoryProperties(queue.device().allocator(),&properties);
    Json result={{"all_allocations",stats.total.statistics.allocationCount},
        {"all_allocation_bytes",stats.total.statistics.allocationBytes},{"all_block_bytes",stats.total.statistics.blockBytes},
        {"device_local_allocation_bytes",0ull},{"device_local_block_bytes",0ull},
        {"host_visible_allocation_bytes",0ull},{"host_visible_block_bytes",0ull}};
    for(unsigned i=0;i<properties->memoryTypeCount;++i) {
        const auto& s=stats.memoryType[i].statistics;
        for(const auto& [flag,prefix]:{std::pair{VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,"device_local"},std::pair{VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,"host_visible"}}) {
            if(!(properties->memoryTypes[i].propertyFlags&flag)) continue;
            const auto a=std::string{prefix}+"_allocation_bytes",b=std::string{prefix}+"_block_bytes";
            result[a]=result[a].get<std::uint64_t>()+s.allocationBytes;
            result[b]=result[b].get<std::uint64_t>()+s.blockBytes;
        }
    }
    return result;
}
Json generations(AtmosphereGenerations g) { return {g.transmittance,g.multiple_scattering,g.sky_view}; }
struct Preset { const char* name; double height,mu,elevation; };
constexpr std::array presets{Preset{"noon",1,.7,.18},Preset{"sunset",1,.015,.1},Preset{"high-altitude",100000,.3,-.15}};
Json environment_json(const EnvironmentDescription& e) {
    const auto& a=e.atmosphere;
    return {{"enabled",e.enabled},{"planet_center_m",a.planet_center_m},{"ground_radius_m",a.ground_radius_m},
        {"atmosphere_height_m",a.atmosphere_height_m},{"rayleigh_scattering_per_m",a.rayleigh_scattering_per_m},
        {"rayleigh_scale_height_m",a.rayleigh_scale_height_m},{"mie_scattering_per_m",a.mie_scattering_per_m},
        {"mie_extinction_per_m",a.mie_extinction_per_m},{"mie_scale_height_m",a.mie_scale_height_m},{"mie_anisotropy",a.mie_anisotropy},
        {"absorption_extinction_per_m",a.absorption_extinction_per_m},{"absorption_center_height_m",a.absorption_center_height_m},
        {"absorption_half_width_m",a.absorption_half_width_m},{"ground_albedo",a.ground_albedo},
        {"sun_direction",e.sun.direction},{"sun_irradiance",e.sun.irradiance},{"sun_angular_radius_rad",e.sun.angular_radius_rad}};
}
void ppm(const std::filesystem::path& path,std::span<const std::byte> pixels,unsigned width,unsigned height) {
    std::ofstream stream(path,std::ios::binary); stream<<"P6\n"<<width<<' '<<height<<"\n255\n";
    for(std::size_t i=0;i<pixels.size();i+=4) stream.write(reinterpret_cast<const char*>(pixels.data()+i),3);
    require(bool(stream),"cannot write preset image");
}
void run(Json& output,const Preset& preset,unsigned width,unsigned height,unsigned samples,bool timestamps,bool validation,const std::filesystem::path& report) {
    auto system=take(memory::MemorySystem::create()); auto heap=take(system.create_heap({"atmosphere-benchmark",memory::DomainCategory::render}));
    Diagnostics diagnostics;
    {
        memory::ThreadContext thread{system}; memory::ExecutionScope scope{thread,heap};
        DeviceOptions options; options.validation=validation ? ValidationMode::required : ValidationMode::disabled;
        options.diagnostic_sink=diagnostic; options.diagnostic_user_data=&diagnostics;
        auto start=Clock::now(); auto queue=take(SubmissionQueue::create(heap,take(Device::create(heap,options)),2));
        output["device_queue_ms"]=elapsed(start);
        const auto& adapter=queue.device().adapter();
        output["device"]={{"name",adapter.properties.deviceName.data()},{"driver",adapter.driver.driverInfo.data()},
            {"driver_version",adapter.properties.driverVersion},{"api_version",adapter.properties.apiVersion}};
        check(queue.configure_gpu_profiling({timestamps}));
        output["memory_device_only"]=memory_sample(queue);
        {
            start=Clock::now(); auto pipeline=take(ScenePipeline::create(heap,queue,DK_RENDER_SHADER_DIR));
            output["pipeline_create_ms"]=elapsed(start);
            start=Clock::now(); auto atmosphere=take(AtmosphereRenderer::create(heap,queue,DK_ATMOSPHERE_SHADER_DIR));
            output["atmosphere_create_ms"]=elapsed(start);
            auto scene=take(SceneDocument::create()); auto assets=take(GpuAssets::create(heap));
            EnvironmentDescription environment; environment.enabled=true;
            environment.sun.direction={std::sqrt(1-preset.mu*preset.mu),preset.mu,0}; environment=take(normalize_environment(environment));
            const Vec3d anchor{0,preset.height,0},forward{std::cos(preset.elevation),std::sin(preset.elevation),0};
            const Vec3d right=forward.cross(Vec3d::UnitY()).normalized(),up=right.cross(forward);
            Mat3d basis; basis.col(0)=right; basis.col(1)=up; basis.col(2)=-forward; const Quatd rotation{basis};
            CpuAsset asset; asset.mesh.id=take(AssetId::generate());
            MeshPrimitive primitive; primitive.positions={{-.14f,-.06f,0},{.14f,-.06f,0},{.14f,.06f,0},{-.14f,.06f,0}};
            primitive.normals.assign(4,Vec3f{0,0,1}); primitive.indices={0,1,2,0,2,3};
            MaterialData material; material.id=take(AssetId::generate()); material.base_color={.8f,.3f,.1f,1}; material.double_sided=true;
            primitive.material=material.id; asset.materials.push_back(material); asset.mesh.primitives.push_back(primitive);
            for(auto [distance,offset]:{std::pair{500.,-.38},std::pair{5000.,.38}}) {
                const auto entity=take(EntityId::generate()); check(scene->create_entity(entity));
                check(scene->set_asset_references(entity,{{asset.mesh.id,AssetKind::mesh}}));
                Trsd t; t.translation=anchor+rotation*Vec3d{offset*distance,0,-distance}; t.rotation=rotation; t.scale=Vec3d::Constant(distance);
                check(scene->set_local_transform(entity,t));
            }
            auto upload=take(assets.upload(queue,asset)); require(take(upload.wait(queue)),"asset upload wait"); upload={};
            RenderSettings settings; settings.exposure=8;
            std::vector<std::byte> pixels(std::size_t(width)*height*4);
            AtmosphereSky sky;
            std::optional<RenderView> initial_view;
            output["preset"]={{"name",preset.name},{"environment",environment_json(environment)},
                {"eye_m",{0,preset.height,0}},{"forward",{forward.x(),forward.y(),forward.z()}},
                {"vertical_fov_rad",1.15},{"near_m",1},{"far_m",10000},{"exposure",8},{"time_seconds",0},
                {"geometry_distances_m",{500,5000}},{"width",width},{"height",height}};
            output["frames"]=Json::array(); output["memory_samples"]=Json::array();
            for(const std::string mode:{"static","sky-update","physical-update","unlit"}) {
                const unsigned count=mode=="physical-update" ? 3 : samples+6;
                for(unsigned i=0;i<count;++i) {
                    const bool lit=mode!="unlit";
                    auto e=environment; e.enabled=lit;
                    Vec3d eye=anchor;
                    if(mode=="sky-update") eye.y()+=.1*(i+1);
                    if(mode=="physical-update") e.atmosphere.rayleigh_scattering_per_m[0]*=1+.01*(i+1);
                    check(scene->set_environment(e));
                    const auto begin=Clock::now();
                    ViewDescription description{width,height}; Trsd camera; camera.translation=eye; camera.rotation=rotation;
                    description.camera_world=take(Transformd::from_trs(camera)); description.projection=Mat4d::Zero();
                    const double y=1/std::tan(1.15/2); description.projection(0,0)=y*height/width; description.projection(1,1)=-y;
                    description.projection(2,2)=10000./(1-10000); description.projection(2,3)=10000./(1-10000); description.projection(3,2)=-1;
                    auto view=take(RenderView::create(take(RenderScene::extract(heap,take(scene->snapshot()))),description));
                    const auto extracted=Clock::now(); const auto old=atmosphere.generations();
                    if(lit) sky=take(atmosphere.prepare(queue,e,eye));
                    const auto prepared=Clock::now();
                    auto frame=take(lit ? pipeline.render_atmosphere(queue,view,assets,sky,settings) : pipeline.render(queue,view,assets,settings));
                    const auto submitted=Clock::now(); require(take(frame.wait(queue)),"frame wait");
                    const auto waited=Clock::now(); check(frame.read_rgba8(pixels)); const auto done=Clock::now();
                    Json luts=Json::object(); const auto before=generations(old),after=generations(atmosphere.generations());
                    for(unsigned index=0;index<3;++index) if(lit && before[index]!=after[index])
                        luts[std::array{"transmittance","multiple_scattering","sky_view"}[index]]=profile(take(sky.lut_profile(static_cast<AtmosphereLut>(index))),timestamps);
                    output["frames"].push_back({{"mode",mode},{"index",i},{"phase",mode=="physical-update" ? "rebuild" : i==0 ? "cold" : i<6 ? "warmup" : "steady"},
                        {"extract_ms",elapsed(begin,extracted)},{"prepare_ms",elapsed(extracted,prepared)},
                        {"record_submit_ms",elapsed(prepared,submitted)},{"wait_ms",elapsed(submitted,waited)},
                        {"readback_ms",elapsed(waited,done)},{"end_to_end_ms",elapsed(begin,done)},
                        {"generations",after},{"lut_builds",std::move(luts)},{"gpu",profile(frame.submission().gpu_profile(),timestamps)}});
                    output["memory_samples"].push_back({{"mode",mode},{"index",i},{"with_frame",memory_sample(queue)}});
                    if(mode=="static" && i==0) {
                        initial_view=view;
                        ppm(report.parent_path()/(report.stem().string()+".ppm"),pixels,width,height);
                    }
                }
            }
            // Separate, untimed diagnostic frame: completion releases Graph temporaries.
            // Sample before wait/collect, with no preceding frame owner, to include AP/HDR.
            sky=take(atmosphere.prepare(queue,environment,anchor));
            {
                auto frame=take(pipeline.render_atmosphere(queue,*initial_view,assets,sky,settings));
                output["memory_inflight_frame"]=memory_sample(queue);
                require(take(frame.wait(queue)),"memory diagnostic wait");
            }
            output["memory_cache_only"]=memory_sample(queue);
        }
        output["memory_after_release"]=memory_sample(queue);
        require(output["memory_after_release"]["all_allocations"]==output["memory_device_only"]["all_allocations"],"GPU resources leaked");
        check(queue.close());
    }
    output["cpu_heap_peak_bytes"]=heap.snapshot().peak_backing_bytes;
    output["cpu_heap_live_allocations"]=heap.snapshot().live_allocations;
    output["diagnostics"]={{"errors",diagnostics.errors.load()},{"warnings",diagnostics.warnings.load()}};
    require(!diagnostics.errors && !diagnostics.warnings && !heap.snapshot().live_allocations,"diagnostics or CPU lifetime failure");
    require(system.try_close().closed(),"memory close failure");
}
}
int main(int argc,char** argv) {
    Json output={{"schema",1},{"status","failed"}}; std::filesystem::path report;
    try {
        std::string name="noon"; unsigned width=1920,height=1080,samples=40; bool timestamps=false,validation=false;
        for(int i=1;i<argc;++i) {
            const std::string_view arg=argv[i];
            if(arg=="--timestamps") timestamps=true;
            else if(arg=="--validation") validation=true;
            else if(arg=="--smoke") { width=160; height=90; samples=2; }
            else if(arg=="--preset" && i+1<argc) name=argv[++i];
            else if(arg=="--output" && i+1<argc) report=argv[++i];
            else throw std::runtime_error("unknown or incomplete benchmark option");
        }
        const auto preset=std::ranges::find_if(presets,[&](auto p) { return p.name==name; });
        require(!report.empty() && preset!=presets.end(),"output and valid preset required");
        if(!report.parent_path().empty()) std::filesystem::create_directories(report.parent_path());
        output.update({{"compiler",DK_BENCH_COMPILER},{"configuration",DK_BENCH_CONFIG},{"timestamps",timestamps},
            {"validation",validation},{"tracy_compiled",profiling::enabled()},{"tracy_connected",profiling::is_connected()},
            {"steady_samples",samples},{"shader_fingerprint",take(shader_fingerprint(DK_RENDER_SHADER_DIR,true))}});
        run(output,*preset,width,height,samples,timestamps,validation,report);
        require(output["shader_fingerprint"]==take(shader_fingerprint(DK_RENDER_SHADER_DIR,true)),"shader sources changed during run");
        output["status"]="passed";
    } catch(const std::exception& error) { output["error"]=error.what(); std::fprintf(stderr,"atmosphere benchmark: %s\n",error.what()); }
    if(!report.empty()) { std::ofstream stream(report); stream<<output.dump(2)<<'\n'; if(!stream) return 1; }
    return output["status"]=="passed" ? 0 : 1;
}

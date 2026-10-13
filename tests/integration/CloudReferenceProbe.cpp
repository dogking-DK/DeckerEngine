#include <dk/render/CloudReferenceRenderer.hpp>
#include <dk/memory/MemorySystem.hpp>
#include "SubmissionInternal.hpp"
#include "../fixtures/CloudReferenceCases.hpp"
#include <atomic>
#include <cstdio>
#include <numbers>
#include <stdexcept>
using namespace dk;
using namespace dk::graphics;
using namespace dk::render;
namespace {
template<class T,class E> T take(std::expected<T,E> r) {
    if(!r) { if constexpr(std::is_same_v<E,Error>) throw std::runtime_error(r.error().message); else throw std::bad_alloc{}; }
    return std::move(*r);
}
void check(Result<void> r) { if(!r) throw std::runtime_error(r.error().message); }
void require(bool ok,const char* text) { if(!ok) throw std::runtime_error(text); }
struct Diagnostics { std::atomic<unsigned> errors=0,warnings=0; };
void diagnostic(void* context,const Diagnostic& message) noexcept {
    auto& d=*static_cast<Diagnostics*>(context);
    if(message.severity==VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) ++d.errors;
    if(message.severity==VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) ++d.warnings;
    if(message.severity>=VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
        std::fprintf(stderr,"%.*s: %.*s\n",int(message.name.size()),message.name.data(),int(message.message.size()),message.message.data());
}
PFN_vkQueueSubmit2 native_submit=nullptr; bool fail_submit=false;
VKAPI_ATTR VkResult VKAPI_CALL submit(VkQueue queue,std::uint32_t count,const VkSubmitInfo2* info,VkFence fence) {
    if(std::exchange(fail_submit,false)) return VK_ERROR_OUT_OF_HOST_MEMORY;
    return native_submit(queue,count,info,fence);
}
std::uint32_t allocations(const SubmissionQueue& queue) {
    VmaTotalStatistics stats{}; vmaCalculateStatistics(queue.device().allocator(),&stats); return stats.total.statistics.allocationCount;
}
void compare(const CloudIntegral& gpu,const CloudIntegral& cpu,double rgb=.02,double trans=.005) {
    require(gpu.radiance.allFinite() && (gpu.radiance.array()>=0).all() && gpu.transmittance>=0 && gpu.transmittance<=1,"unbounded cloud integral");
    require(std::abs(gpu.transmittance-cpu.transmittance)<=trans,"GPU cloud transmittance differs from CPU");
    require(cloud_cases::rgb_error(gpu,cpu)<=rgb,"GPU cloud radiance differs from CPU");
    require(std::abs(gpu.distance_m-cpu.distance_m)<2 && std::abs(gpu.mean_distance_m-cpu.mean_distance_m)<std::max(2.,cpu.distance_m*.005),"GPU cloud distances differ from CPU");
    require(gpu.samples==cpu.samples,"cloud sample count differs");
}
void run(memory::ResourceHandle heap,SubmissionQueue& queue) {
    auto e=cloud_cases::environment(); CloudFieldCache fields; auto data=take(fields.prepare(e.clouds));
    auto cache=take(CloudTextureCache::create(queue)); auto textures=take(cache->prepare(queue,*data));
    auto renderer=take(CloudReferenceRenderer::create(heap,queue,DK_CLOUD_REFERENCE_SHADER_DIR));
    const auto rays=cloud_cases::rays(); auto s=cloud_cases::settings();
    const auto low=take(renderer.integrate(queue,e,*textures,rays,s));
    for(unsigned i=0;i<rays.size();++i) {
        const auto cpu=take(integrate_cloud_reference(e,*data,rays[i],s)); compare(low[i],cpu);
        std::printf("cloud GPU/CPU ray=%u T_abs=%.9g RGB_normalized=%.9g\n",i,std::abs(low[i].transmittance-cpu.transmittance),cloud_cases::rgb_error(low[i],cpu));
    }
    const auto middle=take(renderer.integrate(queue,e,*textures,rays,cloud_cases::settings(1)));
    const auto high=take(renderer.integrate(queue,e,*textures,rays,cloud_cases::settings(2)));
    for(unsigned i=0;i<rays.size();++i) {
        std::printf("cloud GPU convergence ray=%u 256_to_512 T=%.9g RGB=%.9g 512_to_1024 T=%.9g RGB=%.9g\n",i,
            std::abs(low[i].transmittance-middle[i].transmittance),cloud_cases::rgb_error(low[i],middle[i]),
            std::abs(middle[i].transmittance-high[i].transmittance),cloud_cases::rgb_error(middle[i],high[i]));
        require(std::abs(middle[i].transmittance-high[i].transmittance)<=.005 && cloud_cases::rgb_error(middle[i],high[i])<=.02,"GPU reference did not converge");
    }
    // Homogeneous analytic solution uses a constant source: independent of sample placement.
    auto analytic=e; analytic.enabled=false; analytic.sun.direction={0,1,0};
    auto uniform=s; uniform.homogeneous=true; uniform.self_shadow=false; uniform.multiple_scattering=0; uniform.phase_g=0;
    uniform.sky_radiance={.1,.2,.3}; uniform.sun_steps=uniform.atmosphere_steps=8;
    const std::array<CloudRay,1> upward{CloudRay{{0,0,0},{0,1,0},10000}};
    double analytic_error=0;
    for(double extinction:{1e-12,1e-5,.001,1.}) {
        analytic.clouds.extinction_per_m=extinction;
        const auto v=take(renderer.integrate(queue,analytic,*textures,upward,uniform))[0];
        const double trans=std::exp(-extinction*2500),opacity=-std::expm1(-extinction*2500);
        const Vec3d light=(uniform.sky_radiance+Vec3d{e.sun.irradiance.data()}/(4*std::numbers::pi))*(uniform.single_scattering_albedo*opacity);
        analytic_error=std::max({analytic_error,std::abs(v.transmittance-trans),(v.radiance-light).cwiseAbs().maxCoeff()});
    }
    require(analytic_error<=.002,"GPU homogeneous analytic error exceeded bound"); std::printf("cloud GPU homogeneous max_abs=%.9g\n",analytic_error);
    for(unsigned mode=0;mode<7;++mode) {
        auto env=e; auto settings=uniform;
        if(mode==0) env.clouds.enabled=false; if(mode==1) env.clouds.coverage=0; if(mode==2) env.clouds.density=0;
        if(mode==3) env.clouds.extinction_per_m=0; if(mode==4) settings.single_scattering_albedo=0;
        if(mode==5) { env.sun.irradiance={0,0,0}; settings.sky_radiance.setZero(); }
        if(mode==6) { env.sun.direction={0,-1,0}; settings.sky_radiance.setZero(); }
        const auto v=take(renderer.integrate(queue,env,*textures,upward,settings))[0];
        require(v.radiance.isZero(0),"zero or blocked source produced light"); if(mode<4) require(v.transmittance==1,"zero cloud attenuated light");
    }
    // Main shell geometry (including tangent, a ground stop, and two separated intervals).
    const std::array geometry{CloudRay{{0,4000,0},{1,0,0},20000},CloudRay{{0,1500,0},{1,0,0},20000},
        CloudRay{{0,0,0},{0,-1,0},20000},CloudRay{{0,6000,0},{0,1,0},20000},CloudRay{{0,2000,0},{0,1,0},800}};
    auto values=take(renderer.integrate(queue,e,*textures,geometry,uniform));
    for(unsigned i=0;i<geometry.size();++i) compare(values[i],take(integrate_cloud_reference(e,*data,geometry[i],uniform)));
    auto planet=e; planet.enabled=false; planet.atmosphere.planet_center_m={0,0,0}; planet.atmosphere.ground_radius_m=1000;
    planet.clouds.bottom_height_m=100; planet.clouds.top_height_m=200;
    const std::array two{CloudRay{{-2000,1050,0},{1,0,0},4000}}; uniform.self_shadow=true;
    compare(take(renderer.integrate(queue,planet,*textures,two,uniform))[0],take(integrate_cloud_reference(planet,*data,two[0],uniform)));
    auto thin=e; thin.atmosphere.atmosphere_height_m=3000; thin.atmosphere.rayleigh_scale_height_m=1000; thin.atmosphere.mie_scale_height_m=500;
    thin.atmosphere.absorption_center_height_m=thin.atmosphere.absorption_half_width_m=1000;
    thin.clouds.bottom_height_m=1000; thin.clouds.top_height_m=2000;
    compare(take(renderer.integrate(queue,thin,*textures,upward,uniform))[0],take(integrate_cloud_reference(thin,*data,upward[0],uniform)));
    // Keep approximation changes distinct from resolution/parity error.
    for(unsigned mode=0;mode<3;++mode) {
        auto settings=s; if(mode==0) settings.self_shadow=false; if(mode==1) settings.multiple_scattering=0; if(mode==2) settings.time_seconds=-400;
        const auto gpu=take(renderer.integrate(queue,e,*textures,upward,settings))[0]; const auto cpu=take(integrate_cloud_reference(e,*data,upward[0],settings)); compare(gpu,cpu);
        const auto baseline=take(integrate_cloud_reference(e,*data,upward[0],s));
        require(std::abs(gpu.transmittance-baseline.transmittance)>1e-5 || cloud_cases::rgb_error(gpu,baseline)>1e-4,"model or time toggle ineffective");
        std::printf("cloud model/time mode=%u CPU_delta_RGB=%.9g GPU_CPU_RGB=%.9g\n",mode,cloud_cases::rgb_error(cpu,baseline),cloud_cases::rgb_error(gpu,cpu));
    }
    const auto restored=take(renderer.integrate(queue,e,*textures,rays,s));
    for(unsigned i=0;i<rays.size();++i) require(low[i].radiance==restored[i].radiance && low[i].transmittance==restored[i].transmittance,"cloud reference accumulated history");
    const auto live=allocations(queue); const auto submitted=queue.stats().submitted;
    auto invalid=s; invalid.max_work=1; require(!renderer.integrate(queue,e,*textures,rays,invalid),"work budget accepted");
    invalid=s; invalid.max_bytes=1; require(!renderer.integrate(queue,e,*textures,rays,invalid),"byte budget accepted");
    invalid=s; invalid.steps=4097; require(!renderer.integrate(queue,e,*textures,rays,invalid),"unbounded steps accepted");
    std::vector<CloudRay> excess(4097); require(!renderer.integrate(queue,e,*textures,excess,s),"ray budget accepted");
    require(!renderer.integrate(queue,e,*textures,{},s),"empty batch accepted");
    auto unsupported=e; unsupported.clouds.shape_scale_m=1; require(!renderer.integrate(queue,unsupported,*textures,rays,s),"unsupported GPU domain accepted");
    auto wrong=e; ++wrong.clouds.seed; require(!renderer.integrate(queue,wrong,*textures,rays,s),"wrong texture key accepted");
    const std::array bad_ray{CloudRay{{0,-1,0},{0,1,0},1000}}; require(!renderer.integrate(queue,e,*textures,bad_ray,s),"underground ray accepted");
    require(queue.stats().submitted==submitted && allocations(queue)==live,"validation failure allocated or submitted");
    fail_submit=true; require(!renderer.integrate(queue,e,*textures,rays,s) && !fail_submit,"submit fault was not observed");
    require(cache->current()==textures && allocations(queue)==live && !queue.stats().recording_slots,"submit failure changed current or leaked");
    const auto retry=take(renderer.integrate(queue,e,*textures,rays,s));
    require(retry[0].radiance==low[0].radiance && textures->hash()==data->hash(),"failed batch damaged reusable texture or old result");
    DeviceOptions options; options.validation=ValidationMode::required;
    auto foreign=take(SubmissionQueue::create(heap,take(Device::create(heap,options))));
    require(!renderer.integrate(foreign,e,*textures,rays,s),"foreign queue accepted");
    auto foreign_cache=take(CloudTextureCache::create(foreign)); auto foreign_textures=take(foreign_cache->prepare(foreign,*data));
    require(!renderer.integrate(queue,e,*foreign_textures,rays,s),"foreign textures accepted"); foreign_textures.reset(); foreign_cache.reset(); check(foreign.close());
    // New seed produces a distinct result; retained old textures remain usable with their old environment.
    auto next=take(fields.prepare(wrong.clouds)); auto next_textures=take(cache->prepare(queue,*next));
    const auto seeded=take(renderer.integrate(queue,wrong,*next_textures,upward,s))[0]; compare(seeded,take(integrate_cloud_reference(wrong,*next,upward[0],s)));
    require(std::abs(seeded.transmittance-low[0].transmittance)>1e-5,"seed did not affect reference");
    require(take(renderer.integrate(queue,e,*textures,rays,s))[0].radiance==low[0].radiance,"retired texture generation changed");
}
}
int main() {
    Diagnostics diagnostics; auto memory=take(memory::MemorySystem::create()); auto heap=take(memory.create_heap({"cloud-reference",memory::DomainCategory::render}));
    try {
        {
            DeviceOptions options; options.validation=ValidationMode::required; options.diagnostic_sink=diagnostic; options.diagnostic_user_data=&diagnostics;
            auto device=take(Device::create(heap,options)); native_submit=reinterpret_cast<PFN_vkQueueSubmit2>(device.device_proc("vkQueueSubmit2"));
            auto queue=take(graphics::detail::SubmissionAccess::create(heap,std::move(device),3,{submit})); run(heap,queue);
            require(!queue.stats().pending_slots && !queue.stats().recording_slots && allocations(queue)==0,"cloud reference GPU resources leaked"); check(queue.close());
        }
        std::printf("cloud reference validation errors=%u warnings=%u live=%zu\n",diagnostics.errors.load(),diagnostics.warnings.load(),heap.snapshot().live_allocations);
        return !diagnostics.errors && !diagnostics.warnings && !heap.snapshot().live_allocations && memory.try_close().closed() ? 0 : 1;
    } catch(const std::exception& error) { std::fprintf(stderr,"cloud reference failed: %s\n",error.what()); return 1; }
}

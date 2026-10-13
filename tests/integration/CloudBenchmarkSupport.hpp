#pragma once
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
#include <dk/render/CloudReference.hpp>
#include <dk/render/CloudShadowRenderer.hpp>

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
Json environment_json(const EnvironmentDescription& e) {
    const auto& a=e.atmosphere;
    return {{"preset_version",1},{"enabled",e.enabled},{"planet_center_m",a.planet_center_m},{"ground_radius_m",a.ground_radius_m},
        {"atmosphere_height_m",a.atmosphere_height_m},{"rayleigh_scattering_per_m",a.rayleigh_scattering_per_m},
        {"rayleigh_scale_height_m",a.rayleigh_scale_height_m},{"mie_scattering_per_m",a.mie_scattering_per_m},
        {"mie_extinction_per_m",a.mie_extinction_per_m},{"mie_scale_height_m",a.mie_scale_height_m},{"mie_anisotropy",a.mie_anisotropy},
        {"absorption_extinction_per_m",a.absorption_extinction_per_m},{"absorption_center_height_m",a.absorption_center_height_m},
        {"absorption_half_width_m",a.absorption_half_width_m},{"ground_albedo",a.ground_albedo},
        {"sun_direction",e.sun.direction},{"sun_irradiance",e.sun.irradiance},{"sun_angular_radius_rad",e.sun.angular_radius_rad},
        {"clouds",{{"enabled",e.clouds.enabled},{"seed",e.clouds.seed},{"generator_version",e.clouds.generator_version},
        {"weather_resolution",e.clouds.weather_resolution},{"base_resolution",e.clouds.base_resolution},{"detail_resolution",e.clouds.detail_resolution},
        {"bottom_height_m",e.clouds.bottom_height_m},{"top_height_m",e.clouds.top_height_m},{"coverage",e.clouds.coverage},
        {"density",e.clouds.density},{"extinction_per_m",e.clouds.extinction_per_m},{"weather_scale_m",e.clouds.weather_scale_m},
        {"shape_scale_m",e.clouds.shape_scale_m},{"detail_scale_m",e.clouds.detail_scale_m},
        {"bottom_fade_fraction",e.clouds.bottom_fade_fraction},{"top_fade_fraction",e.clouds.top_fade_fraction},{"wind_velocity_mps",e.clouds.wind_velocity_mps}}}};
}
void ppm(const std::filesystem::path& path,std::span<const std::byte> pixels,unsigned width,unsigned height) {
    std::ofstream stream(path,std::ios::binary); stream<<"P6\n"<<width<<' '<<height<<"\n255\n";
    for(std::size_t i=0;i<pixels.size();i+=4) stream.write(reinterpret_cast<const char*>(pixels.data()+i),3);
    require(bool(stream),"cannot write preset image");
}

} // namespace

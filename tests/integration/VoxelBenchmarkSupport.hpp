#pragma once
#include <dk/render/ScenePipeline.hpp>
#include <dk/render/VoxelGpu.hpp>
#include <dk/memory/MemorySystem.hpp>
#include <dk/memory/Context.hpp>
#include <dk/profiling/Profiler.hpp>
#include <dk/scene/SceneDocument.hpp>
#include <dk/math/Projection.hpp>
#include "SubmissionInternal.hpp"
#include "OpenVdbConversion.hpp"
#include <nanovdb/NanoVDB.h>
#include <nlohmann/json.hpp>
#include <Windows.h>
#include <Psapi.h>
#include <atomic>
#include <chrono>
#include <fstream>
#include <numbers>
#include <thread>

using namespace dk;
using namespace dk::render;
using namespace dk::graphics;
using Json=nlohmann::json;
using Clock=std::chrono::steady_clock;
namespace vx=dk::geometry::voxels;
namespace {
template<class T,class E> T take(std::expected<T,E> r) {
    if(!r) { if constexpr(std::is_same_v<E,Error>) throw std::runtime_error(r.error().message); else throw std::bad_alloc{}; }
    return std::move(*r);
}
void require(bool v,const char* s) { if(!v) throw std::runtime_error(s); }
void check(Result<void> r) { if(!r) throw std::runtime_error(r.error().message); }
double ms(Clock::time_point a,Clock::time_point b=Clock::now()) { return std::chrono::duration<double,std::milli>(b-a).count(); }
std::uint64_t hash_bytes(std::span<const std::byte> bytes) {
    std::uint64_t value=14695981039346656037ull;
    for(auto b:bytes) { value^=std::to_integer<unsigned>(b); value*=1099511628211ull; } return value;
}
Json gpu_profile(const Submission& s) {
    const auto p=s.gpu_profile();
    require(p.status()==GpuProfileStatus::ready && !p.dropped_zones() && p.timestamp_valid_bits()>0 && p.timestamp_period_ns()>0,"missing GPU timestamps");
    Json result={{"submission_ms",0.0},{"zones",Json::array()},{"valid_bits",p.timestamp_valid_bits()},{"period_ns",p.timestamp_period_ns()}};
    unsigned count=0;
    for(const auto& t:p.timings()) {
        require(std::isfinite(t.elapsed_ns) && t.elapsed_ns>=0,"invalid GPU interval");
        result["zones"].push_back({{"name",t.name},{"ms",t.elapsed_ns/1e6}});
        if(t.kind==GpuZoneKind::submission) { result["submission_ms"]=t.elapsed_ns/1e6; ++count; }
    }
    require(count==1,"missing GPU submission interval"); return result;
}
Json gpu_memory(const SubmissionQueue& q) {
    VmaTotalStatistics s{}; vmaCalculateStatistics(q.device().allocator(),&s);
    const VkPhysicalDeviceMemoryProperties* props=nullptr; vmaGetMemoryProperties(q.device().allocator(),&props);
    std::uint64_t device=0,host=0;
    for(unsigned i=0;i<props->memoryTypeCount;++i) {
        if(props->memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) device+=s.memoryType[i].statistics.allocationBytes;
        if(props->memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) host+=s.memoryType[i].statistics.allocationBytes;
    }
    return {{"allocations",s.total.statistics.allocationCount},{"allocation_bytes",s.total.statistics.allocationBytes},
        {"block_bytes",s.total.statistics.blockBytes},{"device_local_bytes",device},{"host_visible_bytes",host}};
}
struct ProcessMemory { std::uint64_t working=0,private_bytes=0,peak_working=0,peak_commit=0; };
ProcessMemory process_memory() {
    PROCESS_MEMORY_COUNTERS_EX p{}; p.cb=sizeof(p);
    require(GetProcessMemoryInfo(GetCurrentProcess(),reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&p),sizeof(p))!=0,"process memory query failed");
    return {p.WorkingSetSize,p.PrivateUsage,p.PeakWorkingSetSize,p.PeakPagefileUsage};
}
// A phase sampler is a lower bound on transient peaks. OS peaks cover the whole process.
class MemorySampler {
public:
    MemorySampler() : initial_(process_memory()),peak_(initial_),worker_([this](std::stop_token stop) {
        try { while(!stop.stop_requested()) { sample(); std::this_thread::sleep_for(std::chrono::milliseconds(1)); } }
        catch(...) { failed_=true; }
    }) {}
    Json finish() {
        worker_.request_stop(); worker_.join(); sample(); require(!failed_,"process memory sampler failed");
        return {{"interval_ms",1},{"sampled_working_peak",peak_.working},{"sampled_private_peak",peak_.private_bytes},
            {"working_before",initial_.working},{"private_before",initial_.private_bytes},
            {"process_lifetime_working_peak",peak_.peak_working},{"process_lifetime_commit_peak",peak_.peak_commit},
            {"samples",samples_},{"internal_scratch_exact",false}};
    }
private:
    void sample() { const auto p=process_memory(); peak_.working=std::max(peak_.working,p.working); peak_.private_bytes=std::max(peak_.private_bytes,p.private_bytes);
        peak_.peak_working=std::max(peak_.peak_working,p.peak_working); peak_.peak_commit=std::max(peak_.peak_commit,p.peak_commit); ++samples_; }
    ProcessMemory initial_,peak_; std::uint64_t samples_=0; bool failed_=false; std::jthread worker_;
};
struct Diagnostics { std::atomic<unsigned> errors=0,warnings=0; };
void diagnostic(void* user,const Diagnostic& d) noexcept {
    auto& counts=*static_cast<Diagnostics*>(user);
    if(d.severity==VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) ++counts.errors;
    if(d.severity==VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) ++counts.warnings;
    if(d.severity>=VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) std::fprintf(stderr,"%.*s\n",int(d.message.size()),d.message.data());
}
Transformd transform(const Vec3d& translation,const Vec3d& scale=Vec3d::Ones()) { Trsd t; t.translation=translation; t.scale=scale; return take(Transformd::from_trs(t)); }
int tone(double value) { const double x=value/(1+value); return int(std::lround(255*(x<=.0031308 ? 12.92*x : 1.055*std::pow(x,1/2.4)-.055))); }
void save_json(const std::filesystem::path& p,const Json& j) { std::ofstream f(p); f<<j.dump(2)<<'\n'; require(bool(f),"report write failed"); }
}

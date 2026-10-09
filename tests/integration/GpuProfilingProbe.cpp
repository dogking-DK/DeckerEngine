#include <dk/render/ClothRenderer.hpp>
#include <dk/memory/MemorySystem.hpp>
#include <dk/profiling/Profiler.hpp>
#include "SubmissionInternal.hpp"
#include <nlohmann/json.hpp>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <thread>

using namespace dk;
using namespace dk::graphics;
namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class T, class E> T take(std::expected<T,E>&& result) {
    if (!result) {
        if constexpr (std::is_same_v<E,Error>) throw std::runtime_error(result.error().message);
        else throw std::runtime_error("memory operation failed");
    }
    return std::move(*result);
}
void check(Result<void> result) { if (!result) throw std::runtime_error(result.error().message); }
struct Diagnostics { std::atomic<unsigned> errors=0, warnings=0, loader=0; };
void diagnostic(void* pointer, const Diagnostic& d) noexcept {
    auto& counts = *static_cast<Diagnostics*>(pointer);
    if (d.severity == VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) ++counts.errors;
    if (d.severity == VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        if (d.name == "Loader Message" && d.message.find("VK_LAYER_AMD_switchable_graphics uses API version 1.3") != std::string_view::npos) ++counts.loader;
        else ++counts.warnings;
    }
    if (d.severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
        std::fprintf(stderr, "%.*s: %.*s\n", static_cast<int>(d.name.size()), d.name.data(), static_cast<int>(d.message.size()), d.message.data());
}
PFN_vkQueueSubmit2 native_submit;
PFN_vkWaitSemaphores native_wait;
PFN_vkGetQueryPoolResults native_query;
VkResult submit_error = VK_SUCCESS, query_error = VK_SUCCESS;
VkResult wait_error = VK_SUCCESS;
bool timeout_once = false;
unsigned query_reads = 0;
VKAPI_ATTR VkResult VKAPI_CALL submit_override(VkQueue q, std::uint32_t n, const VkSubmitInfo2* i, VkFence f) {
    if (submit_error != VK_SUCCESS) return std::exchange(submit_error, VK_SUCCESS);
    return native_submit(q,n,i,f);
}
VKAPI_ATTR VkResult VKAPI_CALL wait_override(VkDevice d, const VkSemaphoreWaitInfo* i, std::uint64_t t) {
    if (std::exchange(timeout_once,false)) return VK_TIMEOUT;
    if (wait_error != VK_SUCCESS) {
        // Finish real work before simulating device loss; the mock must not free live GPU objects.
        const auto actual = native_wait(d,i,UINT64_MAX);
        return actual == VK_SUCCESS ? std::exchange(wait_error,VK_SUCCESS) : actual;
    }
    return native_wait(d,i,t);
}
VKAPI_ATTR VkResult VKAPI_CALL query_override(VkDevice d, VkQueryPool p, std::uint32_t first, std::uint32_t count,
    std::size_t size, void* data, VkDeviceSize stride, VkQueryResultFlags flags) {
    ++query_reads;
    if (query_error != VK_SUCCESS) return std::exchange(query_error,VK_SUCCESS);
    return native_query(d,p,first,count,size,data,stride,flags);
}
const char* kind_name(GpuZoneKind kind) {
    switch (kind) {
    case GpuZoneKind::submission: return "submission";
    case GpuZoneKind::compute: return "compute";
    case GpuZoneKind::draw: return "draw";
    case GpuZoneKind::transfer: return "transfer";
    default: return "other";
    }
}
nlohmann::json inspect(const GpuProfile& profile) {
    require(profile.status() == GpuProfileStatus::ready, "profile did not become ready");
    require(profile.timestamp_valid_bits() > 0 && profile.timestamp_period_ns() > 0, "invalid clock metadata");
    nlohmann::json zones = nlohmann::json::array();
    for (const auto& timing : profile.timings()) {
        require(std::isfinite(timing.elapsed_ns) && timing.elapsed_ns >= 0 && timing.cpu_end_ns >= timing.cpu_begin_ns, "invalid duration");
        zones.push_back({{"name",timing.name},{"kind",kind_name(timing.kind)},{"gpu_ns",timing.elapsed_ns},
            {"cpu_begin_ns",timing.cpu_begin_ns},{"cpu_end_ns",timing.cpu_end_ns}});
    }
    return {{"submission",profile.submission_value()},{"timestamp_period_ns",profile.timestamp_period_ns()},
        {"timestamp_valid_bits",profile.timestamp_valid_bits()},{"alignment_window_ns",profile.alignment_window_ns()},
        {"dropped_zones",profile.dropped_zones()},{"zones",std::move(zones)}};
}
struct Workload { GpuParticles particles; std::vector<std::byte> pixels; nlohmann::json profiles = nlohmann::json::array(); };
Workload workload(memory::ResourceHandle heap, SubmissionQueue& queue, bool enabled) {
    DK_PROFILE_ZONE("GPU.Probe.Workload");
    check(queue.configure_gpu_profiling({enabled}));
    auto cpu = take(XpbdSolver::cloth());
    auto gpu = take(GpuXpbdSolver::create(heap,queue,cpu,DK_XPBD_SHADER));
    auto renderer = take(render::ClothRenderer::create(heap,queue,DK_CLOTH_SHADER));
    render::ClothView view; view.width=96; view.height=96; view.image_readback=true;
    Workload output;
    for (unsigned round=0; round<4; ++round) {
        auto frame = take(renderer.render(queue,gpu,10000000,2,view,{true}));
        auto profile = frame.physics().submission().gpu_profile();
        require(profile.status() == (enabled ? GpuProfileStatus::pending : GpuProfileStatus::disabled), "premature profile publication");
        require(profile.timings().empty(), "pending profile exposed timing");
        require(take(frame.physics().wait(queue)), "workload wait failed");
        if (enabled) {
            output.profiles.push_back(inspect(profile));
            bool compute=false, draw=false, transfer=false;
            for (const auto& zone : profile.timings()) {
                compute |= zone.kind == GpuZoneKind::compute;
                draw |= zone.kind == GpuZoneKind::draw;
                transfer |= zone.kind == GpuZoneKind::transfer;
            }
            require(compute && draw && transfer && profile.timings().front().kind == GpuZoneKind::submission && !profile.dropped_zones(), "missing GPU phases");
        }
        output.particles = take(frame.physics().read_particles());
        output.pixels.resize(4ull*view.width*view.height); check(frame.read_rgba8(output.pixels));
        check(cpu.advance(10000000,2));
    }
    double position_error=0, velocity_error=0;
    for (std::size_t i=0;i<cpu.positions().size();++i) {
        const auto a=output.particles.positions[i], b=cpu.positions()[i];
        const auto v=output.particles.velocities[i], w=cpu.velocities()[i];
        for (const auto d:{a.x-b.x,a.y-b.y,a.z-b.z}) position_error=std::max(position_error,std::abs(static_cast<double>(d)));
        for (const auto d:{v.x-w.x,v.y-w.y,v.z-w.z}) velocity_error=std::max(velocity_error,std::abs(static_cast<double>(d)));
    }
    require(position_error < 2e-3 && velocity_error < 2e-2 && gpu.steps()==8, "profiling changed solver correctness");
    return output;
}
Submission empty_submit(SubmissionQueue& queue) {
    auto batch = take(queue.begin());
    check(batch.begin_gpu_zone("probe pass")); check(batch.end_gpu_zone());
    return take(queue.submit(std::move(batch)));
}
GpuProfile lifecycle(memory::ResourceHandle heap, SubmissionQueue& queue) {
    check(queue.configure_gpu_profiling({true,2}));
    require(!queue.configure_gpu_profiling({true,0}), "invalid capacity accepted");
    {
        auto abandoned = take(queue.begin()); check(abandoned.begin_gpu_zone("abandoned"));
        require(!queue.configure_gpu_profiling({false}), "configuration replaced recording query pool");
        require(!queue.close(), "close accepted open batch");
    }
    const auto submitted_before = queue.stats().submitted;
    {
        auto graph = take(graph::Graph::create(heap));
        static_cast<void>(take(graph.add_pass({"failed graph",{},true})));
        const auto plan = take(graph.compile());
        const std::array callbacks{graph::PassCallback{0,[](graph::PassContext&,void*) -> Result<void> {
            return std::unexpected(Error{ErrorCode::conflict,"injected callback failure"});
        }}};
        require(!graph::execute(plan,queue,{{},callbacks,{}}) && queue.stats().submitted==submitted_before && queue.stats().free_slots==2,
            "failed graph published profiling or retained a query slot");
    }
    {
        auto failed = take(queue.begin()); check(failed.begin_gpu_zone("failed submit")); check(failed.end_gpu_zone());
        submit_error=VK_ERROR_OUT_OF_HOST_MEMORY;
        require(!queue.submit(std::move(failed)) && queue.stats().submitted==submitted_before, "failed submission published timing");
    }
    auto first=empty_submit(queue), second=empty_submit(queue);
    const auto read_before=query_reads;
    timeout_once=true;
    require(!take(queue.wait(first,0)) && query_reads==read_before && first.gpu_profile().status()==GpuProfileStatus::pending, "timeout collected pending queries");
    require(!queue.begin() && !queue.configure_gpu_profiling({false}), "pending slots reused");
    require(take(queue.wait(first)), "first slot wait failed");
    const auto old=first.gpu_profile();
    auto third=empty_submit(queue); // slot 0 now follows pending slot 1 in submission order.
    require(take(queue.wait(third)), "third slot wait failed");
    require(second.gpu_profile().status()==GpuProfileStatus::ready && old.timings().size()==2, "query reuse corrupted old results");
    auto missing=empty_submit(queue); query_error=VK_NOT_READY;
    require(take(queue.wait(missing)) && missing.gpu_profile().status()==GpuProfileStatus::unavailable && missing.gpu_profile().timings().empty(), "missing query fabricated zero duration");
    auto failed_read=empty_submit(queue); query_error=VK_ERROR_OUT_OF_HOST_MEMORY;
    require(take(queue.wait(failed_read)) && failed_read.gpu_profile().status()==GpuProfileStatus::unavailable, "query failure changed completion");
    auto batch=take(queue.begin());
    check(batch.begin_gpu_zone("kept"));
    require(!batch.begin_gpu_zone("nested") && !queue.submit(std::move(batch)), "unbalanced zone accepted");
    check(batch.end_gpu_zone()); check(batch.begin_gpu_zone("omitted")); check(batch.end_gpu_zone());
    auto limited=take(queue.submit(std::move(batch))); require(take(queue.wait(limited)), "budget wait failed");
    require(limited.gpu_profile().dropped_zones()==1 && limited.gpu_profile().timings().size()==2, "query budget was not bounded");
    const auto polled = empty_submit(queue);
    const auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds{5};
    while (polled.gpu_profile().status()==GpuProfileStatus::pending && std::chrono::steady_clock::now()<deadline) {
        check(queue.poll()); std::this_thread::yield();
    }
    require(polled.gpu_profile().status()==GpuProfileStatus::ready, "poll did not collect GPU timing");
    check(queue.configure_gpu_profiling({false}));
    auto disabled=empty_submit(queue); require(take(queue.wait(disabled)) && disabled.gpu_profile().status()==GpuProfileStatus::disabled, "disabled captured queries");
    check(queue.configure_gpu_profiling({true,2}));
    auto closing=empty_submit(queue); auto result=closing.gpu_profile(); check(queue.close());
    require(result.status()==GpuProfileStatus::ready && old.timings().size()==2 && queue.stats().pending_slots==0, "close failed to drain profiles");
    return result;
}
} // namespace
int main(int argc,char** argv) {
    try {
        const bool capture=argc>1 && std::string_view{argv[1]}=="--capture";
        const std::filesystem::path report=capture ? (argc>2 ? argv[2] : "gpu-profile.json") : (argc>1 ? argv[1] : "gpu-profile.json");
        if (capture) {
            require(profiling::enabled(), "capture requires a profiling build");
            const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds{20};
            while (!profiling::is_connected() && std::chrono::steady_clock::now()<deadline) std::this_thread::sleep_for(std::chrono::milliseconds{10});
            require(profiling::is_connected(), "capture connection timed out");
        }
        Diagnostics diagnostics; auto system=take(memory::MemorySystem::create());
        auto heap=take(system.create_heap({"GPU profiling",memory::DomainCategory::render}));
        nlohmann::json result;
        GpuProfile surviving;
        {
            memory::ThreadContext thread{system}; memory::ExecutionScope scope{thread,heap};
            DeviceOptions options; options.validation=ValidationMode::required; options.diagnostic_sink=diagnostic; options.diagnostic_user_data=&diagnostics;
            auto device=Device::create(heap,options);
            if (!device) { std::fprintf(stderr,"%s\n",device.error().message.c_str()); return device.error().code==ErrorCode::not_supported || device.error().code==ErrorCode::not_found ? 77 : 1; }
            native_submit=reinterpret_cast<PFN_vkQueueSubmit2>(device->device_proc("vkQueueSubmit2"));
            native_wait=reinterpret_cast<PFN_vkWaitSemaphores>(device->device_proc("vkWaitSemaphores"));
            native_query=reinterpret_cast<PFN_vkGetQueryPoolResults>(device->device_proc("vkGetQueryPoolResults"));
            auto queue=take(graphics::detail::SubmissionAccess::create(heap,std::move(*device),2,{submit_override,nullptr,wait_override,query_override}));
            result["gpu"]=queue.device().adapter().properties.deviceName.data();
            result["driver"]=queue.device().adapter().driver.driverInfo.data();
            auto off=workload(heap,queue,false);
            require(query_reads==0, "disabled workload read queries");
            auto on=workload(heap,queue,true);
            require(off.particles.positions==on.particles.positions && off.particles.velocities==on.particles.velocities && off.pixels==on.pixels, "profiling changed particle or image bytes");
            result["profiles"]=std::move(on.profiles);
            surviving=lifecycle(heap,queue);
        }
        require(surviving.status()==GpuProfileStatus::ready && surviving.timings().size()==2, "profile did not survive queue destruction");
        surviving={};
        // Implicit queue destruction and device-loss shutdown both retire queries.
        for (const bool lost : {false,true}) {
            {
                DeviceOptions options; options.validation=ValidationMode::required; options.diagnostic_sink=diagnostic; options.diagnostic_user_data=&diagnostics;
                auto device=take(Device::create(heap,options));
                native_submit=reinterpret_cast<PFN_vkQueueSubmit2>(device.device_proc("vkQueueSubmit2"));
                native_wait=reinterpret_cast<PFN_vkWaitSemaphores>(device.device_proc("vkWaitSemaphores"));
                native_query=reinterpret_cast<PFN_vkGetQueryPoolResults>(device.device_proc("vkGetQueryPoolResults"));
                auto queue=take(graphics::detail::SubmissionAccess::create(heap,std::move(device),1,{submit_override,nullptr,wait_override,query_override}));
                check(queue.configure_gpu_profiling({true,2}));
                const auto pending=empty_submit(queue); surviving=pending.gpu_profile();
                if (lost) {
                    wait_error=VK_ERROR_DEVICE_LOST;
                    require(!queue.wait(pending) && surviving.status()==GpuProfileStatus::pending && !queue.begin(), "loss did not freeze pending work");
                    require(!queue.close() && queue.stats().closed && queue.stats().completed==0 && queue.stats().pending_slots==0, "lost shutdown fabricated completion");
                }
            }
            require(surviving.status()==(lost ? GpuProfileStatus::device_lost : GpuProfileStatus::ready), "destructor/loss profile state differs");
            surviving={};
        }
        result["status"]="passed"; result["tracy_enabled"]=profiling::enabled(); result["steps"]=8;
        result["query_reads"]=query_reads; result["errors"]=diagnostics.errors.load(); result["warnings"]=diagnostics.warnings.load();
        result["loader_warnings"]=diagnostics.loader.load();
        require(diagnostics.errors==0 && diagnostics.warnings==0 && heap.snapshot().live_allocations==0, "validation error or retained allocations");
        require(system.try_close().closed(), "MemorySystem did not close");
        if (!report.parent_path().empty()) std::filesystem::create_directories(report.parent_path());
        std::ofstream output{report}; output<<result.dump(2)<<'\n'; require(bool(output), "cannot write profile report");
        std::printf("GPU profiling passed; steps=8; profiling=%s\n",profiling::enabled() ? "on" : "off");
        if (capture) {
            const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds{25};
            while (profiling::is_connected() && std::chrono::steady_clock::now()<deadline) std::this_thread::sleep_for(std::chrono::milliseconds{10});
        }
        return 0;
    } catch (const std::exception& error) { std::fprintf(stderr,"GPU profiling probe failed: %s\n",error.what()); return 1; }
}

#include <dk/render/ClothRenderer.hpp>
#include <dk/memory/MemorySystem.hpp>
#include "SubmissionInternal.hpp"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <stdexcept>

using namespace dk;
using namespace dk::graphics;
using namespace dk::render;
namespace {
void require(bool condition,const char* message) { if (!condition) throw std::runtime_error(message); }
template<class T,class E> T take(std::expected<T,E>&& result) {
    if (!result) {
        if constexpr (std::is_same_v<E,Error>) {
            auto text=result.error().message; for (auto& c:result.error().context) text+=" / "+c;
            throw std::runtime_error(text);
        } else throw std::runtime_error("memory allocation failed");
    }
    return std::move(*result);
}
void check(Result<void> r) { if (!r) throw std::runtime_error(r.error().message); }
struct Diagnostics { std::atomic<unsigned> errors=0,warnings=0,loader_warnings=0; };
void diagnostic(void* pointer,const Diagnostic& d) noexcept {
    auto& counts=*static_cast<Diagnostics*>(pointer);
    if (d.severity==VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) ++counts.errors;
    if (d.severity==VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        if (d.name=="Loader Message" && d.message.find("VK_LAYER_AMD_switchable_graphics uses API version 1.3")!=std::string_view::npos) ++counts.loader_warnings;
        else ++counts.warnings;
    }
    if (d.severity>=VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
        std::fprintf(stderr,"%.*s: %.*s\n",static_cast<int>(d.name.size()),d.name.data(),static_cast<int>(d.message.size()),d.message.data());
}
PFN_vkQueueSubmit2 native_submit;
PFN_vkWaitSemaphores native_wait;
VkResult submit_error=VK_SUCCESS;
bool timeout_once=false;
VKAPI_ATTR VkResult VKAPI_CALL submit_override(VkQueue q,std::uint32_t n,const VkSubmitInfo2* i,VkFence f) {
    if (submit_error!=VK_SUCCESS) return std::exchange(submit_error,VK_SUCCESS);
    return native_submit(q,n,i,f);
}
VKAPI_ATTR VkResult VKAPI_CALL wait_override(VkDevice d,const VkSemaphoreWaitInfo* i,std::uint64_t timeout) {
    if (std::exchange(timeout_once,false)) return VK_TIMEOUT;
    return native_wait(d,i,timeout);
}
std::uint32_t allocations(SubmissionQueue& queue) {
    VmaTotalStatistics stats{}; vmaCalculateStatistics(queue.device().allocator(),&stats); return stats.total.statistics.allocationCount;
}
struct Difference { double position=0,velocity=0; };
Difference compare(const GpuParticles& actual,const XpbdSolver& cpu,double ptol,double vtol) {
    require(actual.positions.size()==cpu.positions().size(),"particle count differs"); Difference result;
    for (std::size_t i=0;i<actual.positions.size();++i) {
        auto a=actual.positions[i],b=cpu.positions()[i]; auto v=actual.velocities[i],w=cpu.velocities()[i];
        for (float d:{a.x-b.x,a.y-b.y,a.z-b.z}) result.position=std::max(result.position,std::abs(static_cast<double>(d)));
        for (float d:{v.x-w.x,v.y-w.y,v.z-w.z}) result.velocity=std::max(result.velocity,std::abs(static_cast<double>(d)));
        require(a.inverse_mass==b.inverse_mass && v.padding==0,"GPU changed mass or padding");
        if (a.inverse_mass==0) require(a==b && v==w,"GPU moved a pin");
    }
    std::printf("compare particles=%zu position=%.9g velocity=%.9g\n",actual.positions.size(),result.position,result.velocity);
    require(result.position<=ptol && result.velocity<=vtol,"CPU/GPU tolerance exceeded"); return result;
}
void small_cases(memory::ResourceHandle heap,SubmissionQueue& queue) {
    std::vector<XpbdSolver> cases;
    cases.push_back(take(XpbdSolver::create({1,-9.81f,-100,0},{{0,1,0,1}},{{1,0,0,0}},{})));
    cases.push_back(take(XpbdSolver::create({12,0,-10,0},{{0,1,0,1},{2,1,0,2}},{{},{}},{{0,1,1,1e-4f}})));
    cases.push_back(take(XpbdSolver::create({1,0,0,0},{{0,1,0,1},{1,1,0,1}},{{50,0,0,0},{-50,0,0,0}},{{0,1,1,0}})));
    cases.push_back(take(XpbdSolver::create({1,-9.81f,0,0},{{0,0.001f,0,1}},{{2,-1,0,0}},{})));
    ClothConfig config; config.columns=7; config.rows=5; config.seed=19;
    cases.push_back(take(XpbdSolver::cloth(config))); // Tail workgroup and non-square topology.
    for (auto& cpu:cases) {
        auto gpu=take(GpuXpbdSolver::create(heap,queue,cpu,DK_XPBD_SHADER));
        auto frame=take(gpu.advance(queue,10000000,1,{true,true}));
        require(!frame.read_particles(),"pending particle readback accepted");
        require(take(frame.wait(queue)),"GPU step wait failed");
        check(cpu.advance(10000000)); compare(take(frame.read_particles()),cpu,2e-5,2e-5);
        auto changed_dt=cpu; check(changed_dt.advance(20000000));
        auto cached=take(gpu.advance(queue,20000000,1,{true})); require(take(cached.wait(queue)),"cached plan wait failed");
        compare(take(cached.read_particles()),changed_dt,2e-5,2e-5);
        // Creation from an advanced CPU solver must preserve its original fallback directions.
        auto next=take(GpuXpbdSolver::create(heap,queue,cpu,DK_XPBD_SHADER));
        auto second=take(next.advance(queue,10000000,1,{true})); require(take(second.wait(queue)),"second step wait failed");
        check(cpu.advance(10000000)); compare(take(second.read_particles()),cpu,2e-5,2e-5);
    }
    {
        auto cpu=take(XpbdSolver::create({1,0,-10,0},{{1e6f,1,0,1}},{{1e6f,0,0,0}},{}));
        auto gpu=take(GpuXpbdSolver::create(heap,queue,cpu,DK_XPBD_SHADER));
        auto frame=take(gpu.advance(queue,10000000,1,{true})); require(take(frame.wait(queue)),"divergence wait failed");
        auto read=frame.read_particles();
        require(!read && read.error().code==ErrorCode::invalid_state && gpu.steps()==1,"diagnostic divergence contract differs");
    }
    {
        std::vector<ParticlePosition> positions;
        std::vector<DistanceConstraint> constraints;
        for (unsigned i=0;i<17;++i) { positions.push_back({static_cast<float>(i),1,0,1}); if (i) constraints.push_back({0,i,static_cast<float>(i),0}); }
        auto cpu=take(XpbdSolver::create({32,0,-10,0},positions,std::vector<ParticleVelocity>(17),constraints));
        auto gpu=take(GpuXpbdSolver::create(heap,queue,cpu,DK_XPBD_SHADER));
        const auto submitted=queue.stats().submitted;
        auto rejected=gpu.advance(queue,10000000,8);
        require(!rejected && rejected.error().code==ErrorCode::invalid_argument && gpu.steps()==0 && queue.stats().submitted==submitted,"dense graph budget did not reject before submit");
        auto frame=take(gpu.advance(queue,10000000,0)); require(take(frame.wait(queue)),"budget recovery wait failed");
    }
}
void ppm(const std::filesystem::path& path,const SimulationFrame& frame,const std::vector<std::byte>& pixels) {
    std::ofstream out(path,std::ios::binary); out<<"P6\n"<<frame.width()<<' '<<frame.height()<<"\n255\n";
    for (std::size_t i=0;i<pixels.size();i+=4) out.write(reinterpret_cast<const char*>(pixels.data()+i),3);
    require(bool(out),"image write failed");
}
void cloth(memory::ResourceHandle heap,SubmissionQueue& queue,const std::filesystem::path& directory) {
    const auto started=std::chrono::steady_clock::now();
    ClothConfig config; config.seed=42; auto cpu=take(XpbdSolver::cloth(config)); auto initial=cpu.snapshot();
    auto gpu=take(GpuXpbdSolver::create(heap,queue,cpu,DK_XPBD_SHADER));
    auto renderer=take(ClothRenderer::create(heap,queue,DK_CLOTH_SHADER));
    ClothView view; view.image_readback=true;
    auto first=take(renderer.render(queue,gpu,10000000,0,view,{true,true}));
    require(first.physics().steps()==0,"display advanced physics"); require(take(first.physics().wait(queue)),"initial image wait failed");
    auto before=take(first.physics().read_particles()); compare(before,cpu,0,0);
    std::vector<std::byte> start(view.width*view.height*4),finish(start.size()); check(first.read_rgba8(start));
    ppm(directory/"cloth-initial.ppm",first,start);
    // Invalid requests and partial recording/submission failures leave the old step/data untouched.
    const auto submitted=queue.stats().submitted; const auto live=allocations(queue);
    require(!gpu.advance(queue,999999,1) && !gpu.advance(queue,10000000,9),"invalid step accepted");
    for (int kind=0;kind<3;++kind) {
        auto bad=view; if (kind==0) bad.columns=3; if (kind==1) bad.width=0; if (kind==2) bad.view_projection[0]=NAN;
        require(!renderer.render(queue,gpu,10000000,1,bad),"invalid render accepted");
    }
    GpuParticleConsumer fail{[](GpuParticleGraph& output,void*) -> Result<void> {
        const std::array uses{graph::Use{output.positions,{{vk::PipelineStageFlagBits2::eVertexShader,vk::AccessFlagBits2::eShaderStorageRead}}}};
        const auto index=output.graph.counts().passes;
        auto pass=output.graph.add_pass({"injected failure",uses,true}); if (!pass) return std::unexpected(pass.error());
        output.callbacks.push_back({index,[](graph::PassContext&,void*) -> Result<void> { return std::unexpected(Error{ErrorCode::conflict,"injected recording failure"}); }});
        return {};
    }};
    auto failed=gpu.advance(queue,10000000,1,{},fail);
    require(!failed && failed.error().code==ErrorCode::conflict,"recorder failure not propagated");
    submit_error=VK_ERROR_OUT_OF_HOST_MEMORY;
    require(!renderer.render(queue,gpu,10000000,1,view),"submit failure accepted");
    require(gpu.steps()==0 && queue.stats().submitted==submitted && allocations(queue)==live,"failure published state or leaked");
    auto no_step=take(gpu.advance(queue,10000000,0,{true})); require(take(no_step.wait(queue)),"zero-step recovery wait failed");
    compare(take(no_step.read_particles()),cpu,0,0); no_step={};
    // Batch partition differs from CPU, all 300 steps must agree within the predefined tolerance.
    for (unsigned step=0;step<296;step+=8) {
        auto frame=take(gpu.advance(queue,10000000,8)); require(!frame.read_particles(),"unrequested readback available");
        require(take(frame.wait(queue)),"batch wait failed");
    }
    const auto pre_render=queue.stats().submitted;
    auto last=take(renderer.render(queue,gpu,10000000,4,view,{true,true}));
    require(queue.stats().submitted==pre_render+1 && last.physics().steps()==300,"simulation and render split submissions or wrong step count");
    timeout_once=true; require(!take(last.physics().wait(queue,0)),"timeout not preserved");
    require(!last.read_rgba8(finish),"incomplete image was exposed");
    require(take(last.physics().wait(queue)),"final wait failed");
    check(cpu.advance(10000000,300)); const auto particles=take(last.physics().read_particles());
    const auto difference=compare(particles,cpu,2e-3,2e-2);
    double max_error=0,min_height=1e6;
    for (const auto& c:initial.constraints) {
        auto a=particles.positions[c.a],b=particles.positions[c.b];
        const double x=a.x-b.x,y=a.y-b.y,z=a.z-b.z;
        max_error=std::max(max_error,std::abs(std::sqrt(x*x+y*y+z*z)-c.rest_length));
    }
    for (const auto& p:particles.positions) min_height=std::min(min_height,static_cast<double>(p.y));
    require(min_height>=-1e-6 && min_height<=1e-6 && max_error<0.003,"cloth floor/contact/constraint quality failed");
    bool compute_visible=false,synchronized=false;
    for (const auto& barrier:last.physics().execution()->synchronization()) if (barrier.resource==4) {
        if (barrier.target.access&vk::AccessFlagBits2::eShaderStorageWrite) compute_visible=false;
        // Velocity reconstruction consumes the final corrected positions before visualization.
        if ((barrier.before.stages&vk::PipelineStageFlagBits2::eComputeShader) && (barrier.before.access&vk::AccessFlagBits2::eShaderStorageWrite) &&
            (barrier.target.stages&vk::PipelineStageFlagBits2::eComputeShader) && barrier.target.access==vk::AccessFlagBits2::eShaderStorageRead) compute_visible=true;
        synchronized |= compute_visible && bool(barrier.before.stages&vk::PipelineStageFlagBits2::eComputeShader) &&
            bool(barrier.before.access&vk::AccessFlagBits2::eShaderStorageRead) && bool(barrier.target.stages&vk::PipelineStageFlagBits2::eVertexShader) &&
            bool(barrier.target.access&vk::AccessFlagBits2::eShaderStorageRead);
    }
    require(synchronized,"missing compute-write / velocity-read / vertex-read synchronization chain");
    check(last.read_rgba8(finish)); require(start!=finish,"cloth did not visually change");
    unsigned cyan=0,changed=0;
    for (std::size_t i=0;i<finish.size();i+=4) {
        cyan+=std::to_integer<unsigned>(finish[i+1])>70 && std::to_integer<unsigned>(finish[i+2])>100;
        changed+=start[i]!=finish[i] || start[i+1]!=finish[i+1] || start[i+2]!=finish[i+2];
    }
    require(cyan>1000 && changed>1000,"cloth image coverage too small");
    ppm(directory/"cloth-300.ppm",last,finish);
    std::ofstream plan(directory/"cloth-plan.txt"); plan<<last.physics().plan_text(); require(bool(plan),"plan write failed");
    std::ofstream report(directory/"comparison.json"); report<<std::setprecision(17)
        <<"{\"seed\":42,\"steps\":300,\"dt_ns\":10000000,\"max_position_difference\":"<<difference.position
        <<",\"max_velocity_difference\":"<<difference.velocity<<",\"max_constraint_error\":"<<max_error
        <<",\"min_height\":"<<min_height<<",\"cloth_pixels\":"<<cyan<<",\"changed_pixels\":"<<changed
        <<",\"case_elapsed_seconds\":"<<std::chrono::duration<double>(std::chrono::steady_clock::now()-started).count()<<"}\n";
    require(bool(report),"report write failed");
    require(take(first.physics().read_particles()).positions==before.positions,"later compute changed old frame");
    std::vector<std::byte> old(start.size()); check(first.read_rgba8(old)); require(old==start,"later rendering changed old image");
    auto paused=take(renderer.render(queue,gpu,10000000,0,view,{true})); require(take(paused.physics().wait(queue)),"paused frame wait failed");
    require(gpu.steps()==300 && take(paused.physics().read_particles()).positions==particles.positions,"pause changed state");
    view.image_readback=false;
    auto pending=take(renderer.render(queue,gpu,10000000,1,view)); require(!pending.read_rgba8(finish),"unrequested image readback available");
    auto image=take(pending.color())->share(); const auto ticket=pending.physics().submission();
    pending={}; renderer={}; gpu={}; first={}; last={}; paused={};
    require(allocations(queue)>1 && queue.stats().retained_resources>0,"pending destruction prematurely released working resources");
    require(take(queue.wait(ticket)),"pending destruction wait failed");
    require(allocations(queue)==1 && take(image.state()).initialized,"completion retained temporary resources or lost shared image"); image={};
}
}
int main(int argc,char** argv) {
    try {
        const std::filesystem::path directory=argc>1 ? argv[1] : "xpbd-artifacts"; std::filesystem::create_directories(directory);
        Diagnostics diagnostics; auto system=take(memory::MemorySystem::create());
        auto heap=take(system.create_heap({"GPU XPBD",memory::DomainCategory::render}));
        DeviceOptions options; options.validation=ValidationMode::required; options.diagnostic_sink=diagnostic; options.diagnostic_user_data=&diagnostics;
        auto device=Device::create(heap,options);
        if (!device) { std::fprintf(stderr,"%s\n",device.error().message.c_str()); return device.error().code==ErrorCode::not_found || device.error().code==ErrorCode::not_supported ? 77 : 1; }
        {
            memory::ThreadContext thread{system}; memory::ExecutionScope scope{thread,heap};
            native_submit=reinterpret_cast<PFN_vkQueueSubmit2>(device->device_proc("vkQueueSubmit2"));
            native_wait=reinterpret_cast<PFN_vkWaitSemaphores>(device->device_proc("vkWaitSemaphores"));
            auto queue=take(graphics::detail::SubmissionAccess::create(heap,std::move(*device),3,{submit_override,nullptr,wait_override}));
            std::printf("GPU=%s driver=%s\n",queue.device().adapter().properties.deviceName.data(),queue.device().adapter().driver.driverInfo.data());
            small_cases(heap,queue); require(allocations(queue)==0,"small cases leaked");
            cloth(heap,queue,directory); require(allocations(queue)==0,"cloth case leaked");
            auto cpu=take(XpbdSolver::cloth()); auto gpu=take(GpuXpbdSolver::create(heap,queue,cpu,DK_XPBD_SHADER));
            auto frame=take(gpu.advance(queue,10000000,1)); gpu={}; frame={}; check(queue.close());
            require(allocations(queue)==0 && queue.stats().pending_slots==0,"close did not drain pending resources");
        }
        device=std::unexpected(Error{ErrorCode::invalid_state,"released"});
        std::printf("errors=%u warnings=%u loader_warnings=%u liveAllocations=%zu\n",diagnostics.errors.load(),diagnostics.warnings.load(),diagnostics.loader_warnings.load(),heap.snapshot().live_allocations);
        return diagnostics.errors==0 && diagnostics.warnings==0 && heap.snapshot().live_allocations==0 && system.try_close().closed() ? 0 : 1;
    } catch (const std::exception& error) { std::fprintf(stderr,"GPU XPBD probe failed: %s\n",error.what()); return 1; }
}

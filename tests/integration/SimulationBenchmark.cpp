#include <dk/physics/Xpbd.hpp>
#include <dk/profiling/Profiler.hpp>
#include <nlohmann/json.hpp>
#if DK_BENCH_GPU
#include <dk/render/ClothRenderer.hpp>
#include <dk/memory/MemorySystem.hpp>
#endif
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <thread>

using namespace dk;
using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;
namespace {
void require(bool b, const char* message) { if (!b) throw std::runtime_error(message); }
template<class T, class E> T take(std::expected<T,E>&& r) {
    if (!r) {
        if constexpr (std::is_same_v<E,Error>) throw std::runtime_error(r.error().message);
        else throw std::runtime_error("memory operation failed");
    }
    return std::move(*r);
}
void check(Result<void> r) { if (!r) throw std::runtime_error(r.error().message); }
double ns(Clock::time_point begin, Clock::time_point end = Clock::now()) {
    return std::chrono::duration<double,std::nano>(end-begin).count();
}
Json metrics(const XpbdMetrics& m) {
    return {{"particles",m.particle_count},{"constraints",m.constraint_count},{"colors",m.color_count},
        {"max_constraint_error",m.max_constraint_error},{"rms_constraint_error",m.rms_constraint_error},
        {"max_relative_error",m.max_relative_error},{"max_speed",m.max_speed},
        {"kinetic_energy",m.kinetic_energy},{"gravity_potential_energy",m.gravity_potential_energy},
        {"compliant_energy",m.compliant_energy},{"min_height",m.min_height},
        {"max_penetration",m.max_penetration},{"max_pin_displacement",m.max_pin_displacement}};
}
Json particles(std::span<const ParticlePosition> p, std::span<const ParticleVelocity> v) {
    Json result=Json::array();
    require(p.size()==v.size(),"particle size mismatch");
    for (std::size_t i=0; i<p.size(); ++i) {
        for (const auto value : {p[i].x,p[i].y,p[i].z,p[i].inverse_mass,v[i].x,v[i].y,v[i].z})
            require(std::isfinite(value),"nonfinite particle");
        result.push_back({p[i].x,p[i].y,p[i].z,p[i].inverse_mass,v[i].x,v[i].y,v[i].z});
    }
    return result;
}
std::string phase(unsigned first, unsigned count) {
    if (!first) return "cold";
    if (count!=8) return "tail";
    return first<32 ? "warmup" : "steady";
}
#if DK_BENCH_GPU
using namespace dk::graphics;
struct Diagnostics { std::atomic<unsigned> errors=0,warnings=0,loader=0; };
void diagnostic(void* p,const Diagnostic& d) noexcept {
    auto& c=*static_cast<Diagnostics*>(p);
    if (d.severity==VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) ++c.errors;
    if (d.severity==VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        if (d.name=="Loader Message" && d.message.find("VK_LAYER_AMD_switchable_graphics uses API version 1.3")!=std::string_view::npos) ++c.loader;
        else ++c.warnings;
    }
}
Json profile(const GpuProfile& p, bool enabled) {
    if (!enabled) {
        require(p.status()==GpuProfileStatus::disabled,"unexpected GPU acquisition");
        return nullptr;
    }
    require(p.status()==GpuProfileStatus::ready && p.dropped_zones()==0,"missing GPU observations");
    Json result={{"submission_id",p.submission_value()},{"timestamp_period_ns",p.timestamp_period_ns()},
        {"timestamp_valid_bits",p.timestamp_valid_bits()},{"alignment_window_ns",p.alignment_window_ns()},
        {"submission_ns",0.0},{"compute_ns",0.0},{"draw_ns",0.0},{"transfer_ns",0.0},{"other_ns",0.0},
        {"zones",p.timings().size()}};
    for (const auto& t:p.timings()) {
        require(std::isfinite(t.elapsed_ns) && t.elapsed_ns>=0,"invalid timestamp");
        const char* key="other_ns";
        switch (t.kind) {
        case GpuZoneKind::submission: key="submission_ns"; break;
        case GpuZoneKind::compute: key="compute_ns"; break;
        case GpuZoneKind::draw: key="draw_ns"; break;
        case GpuZoneKind::transfer: key="transfer_ns"; break;
        default: break;
        }
        result[key]=result[key].get<double>()+t.elapsed_ns;
    }
    require(p.timestamp_valid_bits()>0 && p.timestamp_period_ns()>0 && !p.timings().empty(),"missing GPU clock");
    return result;
}
void gpu_run(Json& output, const XpbdSolver& initial, unsigned size, unsigned steps, bool timestamps, bool validation) {
    Diagnostics counts;
    auto system=take(memory::MemorySystem::create());
    auto heap=take(system.create_heap({"Simulation benchmark",memory::DomainCategory::render}));
    {
        memory::ThreadContext thread{system}; memory::ExecutionScope scope{thread,heap};
        DeviceOptions options; options.validation=validation ? ValidationMode::required : ValidationMode::disabled;
        options.diagnostic_sink=diagnostic; options.diagnostic_user_data=&counts;
        auto begin=Clock::now();
        auto queue=take(SubmissionQueue::create(heap,take(Device::create(heap,options)),2));
        output["device_queue_ns"]=ns(begin);
        output["device"]={{"name",queue.device().adapter().properties.deviceName.data()},
            {"driver",queue.device().adapter().driver.driverInfo.data()},
            {"api_version",queue.device().adapter().properties.apiVersion}};
        begin=Clock::now(); check(queue.configure_gpu_profiling({timestamps})); output["acquisition_setup_ns"]=ns(begin);
        begin=Clock::now(); auto gpu=take(GpuXpbdSolver::create(heap,queue,initial,DK_XPBD_SHADER));
        output["gpu_initialize_ns"]=ns(begin);
        for (unsigned first=0; first<steps; first+=8) {
            DK_PROFILE_ZONE("benchmark.solve_batch"); DK_PROFILE_ZONE_VALUE(first);
            const auto count=std::min(8u,steps-first);
            const auto start=Clock::now();
            auto frame=take(gpu.advance(queue,10000000,count));
            const auto submitted=Clock::now();
            require(take(frame.wait(queue)),"wait timeout");
            const auto done=Clock::now();
            output["batches"].push_back({{"first_step",first},{"count",count},{"phase",phase(first,count)},
                {"advance_ns",ns(start,submitted)},{"wait_ns",ns(submitted,done)},{"wall_ns",ns(start,done)},
                {"gpu",profile(frame.submission().gpu_profile(),timestamps)}});
        }
        require(gpu.steps()==steps,"incorrect committed steps");
        begin=Clock::now();
        auto readback=take(gpu.advance(queue,10000000,0,{true})); require(take(readback.wait(queue)),"readback wait");
        auto actual=take(readback.read_particles()); output["readback_ns"]=ns(begin);
        output["particles"]=particles(actual.positions,actual.velocities);
        const auto measured=take(initial.evaluate(actual.positions,actual.velocities));
        output["metrics"]=metrics(measured);
        require(measured.max_pin_displacement==0 && measured.max_penetration<=1e-6,"pin/floor contract");
        auto reference=initial; check(reference.advance(10000000,steps));
        double position=0,velocity=0;
        for (std::size_t i=0;i<actual.positions.size();++i) {
            const auto a=actual.positions[i],b=reference.positions()[i];
            const auto v=actual.velocities[i],w=reference.velocities()[i];
            for (auto d:{a.x-b.x,a.y-b.y,a.z-b.z}) position=std::max(position,std::abs(static_cast<double>(d)));
            for (auto d:{v.x-w.x,v.y-w.y,v.z-w.z}) velocity=std::max(velocity,std::abs(static_cast<double>(d)));
        }
        output["reference_metrics"]=metrics(reference.metrics());
        output["difference"]={{"position",position},{"velocity",velocity}};
        require(position<=2e-3 && velocity<=2e-2,"CPU/GPU tolerance exceeded");
        begin=Clock::now(); auto renderer=take(render::ClothRenderer::create(heap,queue,DK_CLOTH_SHADER));
        output["renderer_initialize_ns"]=ns(begin);
        render::ClothView view; view.columns=size; view.rows=size; view.width=256; view.height=256;
        const auto scale=7.0f/static_cast<float>(size-1);
        view.view_projection={1.2f*scale,0,0.4f*scale,-0.2f, 0,-0.9f,0.65f*scale,0.15f,
            0,-0.325f,-0.45f*scale,0.8f, 0,0,0,1};
        output["draws"]=Json::array();
        for (unsigned i=0;i<8;++i) {
            DK_PROFILE_ZONE("benchmark.draw_frame"); DK_PROFILE_ZONE_VALUE(i);
            const auto start=Clock::now(); auto frame=take(renderer.render(queue,gpu,10000000,0,view));
            const auto submitted=Clock::now(); require(take(frame.physics().wait(queue)),"draw wait");
            const auto done=Clock::now();
            output["draws"].push_back({{"phase",i ? "steady" : "cold"},{"advance_ns",ns(start,submitted)},
                {"wait_ns",ns(submitted,done)},{"wall_ns",ns(start,done)},
                {"gpu",profile(frame.physics().submission().gpu_profile(),timestamps)}});
        }
        auto unchanged=take(gpu.advance(queue,10000000,0,{true})); require(take(unchanged.wait(queue)),"final wait");
        auto final=take(unchanged.read_particles());
        require(gpu.steps()==steps && final.positions==actual.positions && final.velocities==actual.velocities,"draw changed simulation");
        check(queue.close());
    }
    output["diagnostics"]={{"errors",counts.errors.load()},{"warnings",counts.warnings.load()},{"loader",counts.loader.load()}};
    require(counts.errors==0 && counts.warnings==0 && heap.snapshot().live_allocations==0,"validation or lifetime failure");
    require(system.try_close().closed(),"MemorySystem close failed");
}
#endif
} // namespace
int main(int argc,char** argv) {
    Json output={{"schema",1},{"status","failed"}};
    std::filesystem::path report;
    try {
        unsigned size=8,steps=300; bool gpu=false,timestamps=false,capture=false,validation=false;
        for (int i=1;i<argc;++i) {
            const std::string_view arg=argv[i];
            if (arg=="--gpu") gpu=true;
            else if (arg=="--timestamps") timestamps=true;
            else if (arg=="--capture") capture=true;
            else if (arg=="--validation") validation=true;
            else if ((arg=="--size" || arg=="--steps" || arg=="--output") && i+1<argc) {
                if (arg=="--output") report=argv[++i];
                else {
                    const std::string value=argv[++i]; std::size_t end=0; const auto n=std::stoul(value,&end);
                    require(end==value.size() && n<=300,"invalid numeric option");
                    (arg=="--size" ? size : steps)=static_cast<unsigned>(n);
                }
            } else throw std::runtime_error("unknown or incomplete benchmark option");
        }
        require(!report.empty() && (size==8 || size==16 || size==32) && steps>=40 && steps<=300,"invalid benchmark configuration");
        require(gpu || (!timestamps && !validation),"GPU options require --gpu");
        require(!capture || profiling::enabled(),"capture requires profiling build");
        if (capture) {
            const auto deadline=Clock::now()+std::chrono::seconds{20};
            while (!profiling::is_connected() && Clock::now()<deadline) std::this_thread::sleep_for(std::chrono::milliseconds{10});
            require(profiling::is_connected(),"Tracy connection timeout");
        }
        output.update({{"backend",gpu ? "gpu" : "cpu"},{"size",size},{"steps",steps},{"dt_ns",10000000},
            {"seed",42},{"iterations",12},{"spacing",0.15},{"timestamps",timestamps},{"validation",validation},
            {"tracy_compiled",profiling::enabled()},{"tracy_connected",profiling::is_connected()},
            {"compiler",DK_BENCH_COMPILER},{"configuration",DK_BENCH_CONFIG},{"batches",Json::array()}});
        ClothConfig config; config.columns=size; config.rows=size; config.seed=42;
        config.spacing=0.15f; config.height=0.75f; config.particle_mass=0.1f; config.compliance=1e-6f;
        config.physics={12,-9.81f,0,0.5f};
        output["cloth"]={{"columns",config.columns},{"rows",config.rows},{"seed",config.seed},
            {"spacing",config.spacing},{"height",config.height},{"particle_mass",config.particle_mass},
            {"compliance",config.compliance},{"iterations",config.physics.iterations},
            {"gravity_y",config.physics.gravity_y},{"floor_y",config.physics.floor_y},{"damping",config.physics.damping}};
        auto start=Clock::now(); auto cpu=take(XpbdSolver::cloth(config)); output["cpu_initialize_ns"]=ns(start);
        if (gpu) {
#if DK_BENCH_GPU
            gpu_run(output,cpu,size,steps,timestamps,validation);
#else
            throw std::runtime_error("GPU backend not built");
#endif
        } else {
            for (unsigned first=0;first<steps;first+=8) {
                DK_PROFILE_ZONE("benchmark.cpu_batch"); DK_PROFILE_ZONE_VALUE(first);
                const auto count=std::min(8u,steps-first); start=Clock::now(); check(cpu.advance(10000000,count));
                const auto done=Clock::now();
                output["batches"].push_back({{"first_step",first},{"count",count},{"phase",phase(first,count)},
                    {"wall_ns",ns(start,done)},{"gpu",nullptr}});
            }
            output["particles"]=particles(cpu.positions(),cpu.velocities()); output["metrics"]=metrics(cpu.metrics());
            require(cpu.metrics().max_pin_displacement==0 && cpu.metrics().max_penetration<=1e-6,"CPU pin/floor contract");
        }
        if (capture) require(profiling::is_connected(),"Tracy disconnected during measurement");
        output["status"]="passed";
    } catch (const std::exception& e) { output["error"]=e.what(); std::fprintf(stderr,"%s\n",e.what()); }
    if (!report.empty()) {
        if (!report.parent_path().empty()) std::filesystem::create_directories(report.parent_path());
        std::ofstream file{report}; file<<output.dump(2)<<'\n';
        if (!file) return 1;
    }
    return output["status"]=="passed" ? 0 : 1;
}

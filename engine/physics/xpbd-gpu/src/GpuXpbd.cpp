#include <dk/profiling/Profiler.hpp>
#include <dk/physics/GpuXpbd.hpp>
#include <dk/graphics/GraphDiagnostics.hpp>
#include <dk/graphics/ShaderCompiler.hpp>
#include <dk/memory/SmartPtr.hpp>
#include <algorithm>
#include <cmath>
#include <deque>

namespace dk {
using namespace graphics;
namespace detail {
struct GpuXpbdState {
    memory::ResourceHandle heap;
    ResourceFactory factory;
    XpbdConfig config;
    std::uint32_t particles = 0, constraints = 0;
    std::uint64_t steps = 0;
    std::vector<std::uint32_t> colors;
    std::array<Buffer,4> buffers; // Current positions, velocities, immutable constraints and directions.
    std::array<PipelineLayout,4> layouts;
    std::array<ComputePipeline,4> pipelines;
    graph::CompiledGraph cached_plan;
    std::uint32_t cached_count = 0;
    bool cached_readback = false;
};
struct GpuXpbdFrameState {
    std::uint64_t steps = 0;
    std::uint32_t particles = 0;
    bool readback = false;
    graph::Execution execution;
    graph::PlanReport report;
};
}
namespace {
template<class T> T take(Result<T> result) { if (!result) throw std::move(result.error()); return std::move(*result); }
void check(Result<void> result) { if (!result) throw std::move(result.error()); }
constexpr auto storage = vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eTransferSrc | vk::BufferUsageFlagBits::eTransferDst;
constexpr auto compute = vk::PipelineStageFlagBits2::eComputeShader;
constexpr auto read = vk::AccessFlagBits2::eShaderStorageRead;
constexpr auto write = vk::AccessFlagBits2::eShaderStorageWrite;
graph::Use use(graph::BufferId id, vk::PipelineStageFlags2 stages, vk::AccessFlags2 access, bool overwrite = false) {
    return {id,{{stages,access},0,VK_WHOLE_SIZE,{},overwrite}};
}
struct Parameters {
    std::uint32_t count, first, end, unused = 0;
    float dt, gravity, floor, damping;
};
static_assert(sizeof(Parameters)==32);
struct Dispatch {
    dk::detail::GpuXpbdState* state;
    unsigned kernel;
    Parameters parameters;
};
Result<void> record_dispatch(graph::PassContext& pass, void* pointer) {
    try {
        auto& c = *static_cast<Dispatch*>(pointer);
        auto& layout = c.state->layouts[c.kernel];
        // Shader binding -> Graph resource index; no guessed descriptor access.
        constexpr std::array<std::size_t,6> resources{4,5,6,2,3,7};
        std::vector<BindingWrite> writes;
        for (const auto& binding : layout.bindings())
            writes.push_back({binding.binding,0,BufferBinding{take(pass.buffer(resources.at(binding.binding)))}});
        auto bindings = take(c.state->factory.create_bindings(layout,0,writes));
        auto encoder = take(pass.compute());
        check(encoder.bind_pipeline(c.state->pipelines[c.kernel]));
        const std::array sets{&bindings}; check(encoder.bind_sets(sets));
        check(encoder.push_constants(vk::ShaderStageFlagBits::eCompute,0,std::as_bytes(std::span{&c.parameters,1})));
        const auto size = c.kernel==1 ? c.parameters.end-c.parameters.first : c.parameters.count;
        return encoder.dispatch({(size+63)/64,1,1});
    } catch (Error& error) { return std::unexpected(std::move(error)); }
}
Error allocation_error() { return {ErrorCode::internal_error,"GPU XPBD allocation failed; no state published"}; }
}
Result<GpuXpbdSolver> GpuXpbdSolver::create(memory::ResourceHandle heap, SubmissionQueue& queue,
    const XpbdSolver& cpu, const std::filesystem::path& shader) {
    DK_PROFILE_ZONE("physics.gpu.initialize");
    if (!heap || heap.state()!=memory::ResourceState::open || queue.stats().closed || queue.stats().device_lost)
        return std::unexpected(Error{ErrorCode::invalid_state,"GPU XPBD requires an open heap and queue"});
    try {
        auto data = cpu.snapshot();
        if (data.positions.empty()) return std::unexpected(Error{ErrorCode::invalid_argument,"empty CPU solver"});
        auto state = memory::make_shared_in<dk::detail::GpuXpbdState>(heap);
        state->heap=heap; state->factory=queue.resources(); state->config=data.config;
        state->particles=static_cast<std::uint32_t>(data.positions.size());
        state->constraints=static_cast<std::uint32_t>(data.constraints.size()); state->colors=std::move(data.color_offsets);
        constexpr std::array defines{"DK_PREDICT","DK_PROJECT","DK_FLOOR","DK_VELOCITY"};
        for (std::size_t k=0;k<defines.size();++k) {
            const std::array definitions{ShaderDefine{defines[k],"1"}};
            auto compiled=take(compile_shader({shader,"computeMain",ShaderStage::compute,{},definitions},heap));
            auto module=take(state->factory.create_shader(compiled));
            const std::array shaders{&module};
            state->layouts[k]=take(state->factory.create_pipeline_layout(shaders));
            state->pipelines[k]=take(state->factory.create_compute_pipeline({&module,&state->layouts[k]}));
        }
        std::vector<std::array<float,4>> directions;
        for (auto d:data.initial_directions) directions.push_back({d[0],d[1],d[2],0});
        // Nonempty descriptor storage even for the zero-constraint free-fall case.
        if (data.constraints.empty()) { data.constraints.push_back({0,0,0,0}); directions.push_back({0,0,0,0}); }
        const std::array bytes{std::as_bytes(std::span{data.positions}),std::as_bytes(std::span{data.velocities}),
            std::as_bytes(std::span{data.constraints}),std::as_bytes(std::span{directions})};
        auto graph=take(graph::Graph::create(heap));
        std::array<Buffer,4> uploads;
        std::array<graph::BufferId,4> sources,targets;
        std::array<graph::ExternalBinding,4> external;
        std::vector<graph::Use> uses;
        for (std::size_t i=0;i<4;++i) {
            uploads[i]=take(queue.create_buffer({bytes[i].size(),vk::BufferUsageFlagBits::eTransferSrc,BufferMemory::upload}));
            check(uploads[i].write(0,bytes[i]));
            sources[i]=take(graph.declare_buffer("XPBD upload "+std::to_string(i),uploads[i].description(),graph::Lifetime::external,true));
            external[i]={i,&uploads[i]};
            uses.push_back(use(sources[i],vk::PipelineStageFlagBits2::eCopy,vk::AccessFlagBits2::eTransferRead));
        }
        for (std::size_t i=0;i<4;++i) {
            targets[i]=take(graph.declare_buffer("XPBD initial "+std::to_string(i),{bytes[i].size(),storage}));
            uses.push_back(use(targets[i],vk::PipelineStageFlagBits2::eCopy,vk::AccessFlagBits2::eTransferWrite,true));
            check(graph.mark_output(targets[i]));
        }
        static_cast<void>(take(graph.add_pass({"XPBD initial upload",uses})));
        const std::array callbacks{graph::PassCallback{0,[](graph::PassContext& pass,void*) -> Result<void> {
            for (std::size_t i=0;i<4;++i) {
                auto buffer=pass.buffer(i); if (!buffer) return std::unexpected(buffer.error());
                if (auto r=pass.copy_buffer(i,i+4,(*buffer)->size()); !r) return r;
            }
            return {};
        }}};
        auto plan=take(graph.compile());
        auto execution=take(graph::execute(plan,queue,{external,callbacks,{}}));
        for (std::size_t i=0;i<4;++i) state->buffers[i]=take(execution.buffer(i+4))->share();
        return GpuXpbdSolver{std::move(state)};
    } catch (Error& error) { return std::unexpected(std::move(error)); }
    catch (const std::bad_alloc&) { return std::unexpected(allocation_error()); }
}
Result<GpuXpbdFrame> GpuXpbdSolver::advance(SubmissionQueue& queue,std::int64_t dt_ns,std::uint32_t count,
    GpuXpbdOptions options,GpuParticleConsumer consumer) {
    DK_PROFILE_ZONE("physics.gpu.advance");
    if (!state_ || state_->heap.state()!=memory::ResourceState::open || queue.stats().closed || queue.stats().device_lost)
        return std::unexpected(Error{ErrorCode::invalid_state,"GPU XPBD solver, heap or queue unavailable"});
    const auto colors=state_->colors.size()-1;
    if (dt_ns<1000000 || dt_ns>33333333 || count>8 || std::uint64_t{count}*(3+state_->config.iterations*(colors+1))>4096 ||
        count>UINT64_MAX-state_->steps)
        return std::unexpected(Error{ErrorCode::invalid_argument,"GPU XPBD dt/count/pass budget or step overflow"});
    try {
        auto frame=memory::make_shared_in<dk::detail::GpuXpbdFrameState>(state_->heap);
        frame->steps=state_->steps+count; frame->particles=state_->particles; frame->readback=options.readback;
        auto graph=take(graph::Graph::create(state_->heap));
        std::array<graph::BufferId,10> ids;
        std::array<graph::ExternalBinding,4> external;
        std::vector<graph::PassCallback> callbacks;
        std::vector<graph::FinalAccess> finals;
        std::deque<Dispatch> dispatches; // Stable callback addresses while building the graph.
        {
        DK_PROFILE_ZONE("physics.gpu.graph_build");
        constexpr std::array names{"previous positions","previous velocities","distance constraints","initial directions"};
        for (std::size_t i=0;i<4;++i) {
            ids[i]=take(graph.declare_buffer(names[i],state_->buffers[i].description(),graph::Lifetime::external,true));
            external[i]={i,&state_->buffers[i]};
        }
        const auto bytes=vk::DeviceSize{state_->particles}*sizeof(ParticlePosition);
        ids[4]=take(graph.declare_buffer("particle positions",{bytes,storage}));
        ids[5]=take(graph.declare_buffer("particle velocities",{bytes,storage}));
        ids[6]=take(graph.declare_buffer("prediction history",{bytes,storage}));
        ids[7]=take(graph.declare_buffer("constraint lambda",{vk::DeviceSize{std::max(1u,state_->constraints)}*4,storage}));
        check(graph.mark_output(ids[4])); check(graph.mark_output(ids[5]));
        if (options.readback) for (std::size_t i=8;i<10;++i) {
            ids[i]=take(graph.declare_buffer(i==8 ? "position readback" : "velocity readback",{bytes,vk::BufferUsageFlagBits::eTransferDst,BufferMemory::readback}));
            check(graph.mark_output(ids[i]));
        }
        auto add=[&](std::string_view name,std::initializer_list<graph::Use> uses,graph::PassRecorder recorder,void* context=nullptr) {
            auto index=graph.counts().passes;
            static_cast<void>(take(graph.add_pass({std::string{name}+" #"+std::to_string(index),{uses.begin(),uses.size()}})));
            callbacks.push_back({index,recorder,context});
        };
        add("XPBD candidate copy",{use(ids[0],vk::PipelineStageFlagBits2::eCopy,vk::AccessFlagBits2::eTransferRead),
            use(ids[1],vk::PipelineStageFlagBits2::eCopy,vk::AccessFlagBits2::eTransferRead),
            use(ids[4],vk::PipelineStageFlagBits2::eCopy,vk::AccessFlagBits2::eTransferWrite,true),
            use(ids[5],vk::PipelineStageFlagBits2::eCopy,vk::AccessFlagBits2::eTransferWrite,true)},
            [](graph::PassContext& pass,void*) -> Result<void> {
                auto p=pass.buffer(0); if (!p) return std::unexpected(p.error());
                auto result=pass.copy_buffer(0,4,(*p)->size()); if (!result) return result;
                return pass.copy_buffer(1,5,(*p)->size());
            });
        Parameters parameters{state_->particles,0,0,0,static_cast<float>(dt_ns)*1e-9f,
            state_->config.gravity_y,state_->config.floor_y,state_->config.damping};
        auto dispatch=[&](std::string_view name,unsigned kernel,std::initializer_list<graph::Use> uses) {
            dispatches.push_back({state_.get(),kernel,parameters}); add(name,uses,record_dispatch,&dispatches.back());
        };
        for (std::uint32_t step=0;step<count;++step) {
            add("XPBD reset lambda",{use(ids[7],vk::PipelineStageFlagBits2::eClear,vk::AccessFlagBits2::eTransferWrite,true)},
                [](graph::PassContext& pass,void*) { return pass.fill(7); });
            dispatch("XPBD predict",0,{use(ids[4],compute,read|write),use(ids[5],compute,read|write),use(ids[6],compute,write,true)});
            for (std::uint32_t iteration=0;iteration<state_->config.iterations;++iteration) {
                for (std::size_t color=0;color<colors;++color) {
                    parameters.first=state_->colors[color]; parameters.end=state_->colors[color+1];
                    dispatch("XPBD color "+std::to_string(color),1,{use(ids[4],compute,read|write),use(ids[2],compute,read),
                        use(ids[3],compute,read),use(ids[7],compute,read|write)});
                }
                dispatch("XPBD floor",2,{use(ids[4],compute,read|write)});
            }
            dispatch("XPBD velocity",3,{use(ids[4],compute,read),use(ids[6],compute,read),use(ids[5],compute,write)});
        }
        if (consumer.append) {
            GpuParticleGraph output{graph,ids[4],4,state_->particles,callbacks,finals};
            check(consumer.append(output,consumer.context));
        }
        if (options.readback) {
            add("XPBD diagnostic readback",{use(ids[4],vk::PipelineStageFlagBits2::eCopy,vk::AccessFlagBits2::eTransferRead),
                use(ids[5],vk::PipelineStageFlagBits2::eCopy,vk::AccessFlagBits2::eTransferRead),
                use(ids[8],vk::PipelineStageFlagBits2::eCopy,vk::AccessFlagBits2::eTransferWrite,true),
                use(ids[9],vk::PipelineStageFlagBits2::eCopy,vk::AccessFlagBits2::eTransferWrite,true)},
                [](graph::PassContext& pass,void*) -> Result<void> {
                    auto p=pass.buffer(4); if (!p) return std::unexpected(p.error());
                    auto result=pass.copy_buffer(4,8,(*p)->size()); if (!result) return result;
                    return pass.copy_buffer(5,9,(*p)->size());
                });
            for (std::size_t i=8;i<10;++i) finals.push_back({i,{{vk::PipelineStageFlagBits2::eHost,vk::AccessFlagBits2::eHostRead}}});
        }
        }
        graph::CompiledGraph candidate_plan;
        const bool cached=!consumer.append && state_->cached_plan && state_->cached_count==count && state_->cached_readback==options.readback;
        if (!cached) candidate_plan=take(graph.compile());
        const auto& plan=cached ? state_->cached_plan : candidate_plan;
        if (options.capture_plan) frame->report=take(graph::format_plan(plan));
        std::vector<graph::ExternalBinding> retained;
        for (const auto& binding:external) if (plan.resources()[binding.resource].retained) retained.push_back(binding);
        frame->execution=take(graph::execute(plan,queue,{retained,callbacks,finals,options.capture_plan}));
        // All allocations and failure-prone operations precede submission. Output indices are guaranteed by this graph.
        state_->buffers[0]=frame->execution.buffer(4).value()->share();
        state_->buffers[1]=frame->execution.buffer(5).value()->share();
        state_->steps=frame->steps;
        if (!consumer.append && !cached) {
            state_->cached_plan=std::move(candidate_plan); state_->cached_count=count; state_->cached_readback=options.readback;
        }
        return GpuXpbdFrame{std::move(frame)};
    } catch (Error& error) { return std::unexpected(std::move(error)); }
    catch (const std::bad_alloc&) { return std::unexpected(allocation_error()); }
}
std::uint64_t GpuXpbdSolver::steps() const noexcept { return state_ ? state_->steps : 0; }
std::uint32_t GpuXpbdSolver::particle_count() const noexcept { return state_ ? state_->particles : 0; }
std::uint64_t GpuXpbdFrame::steps() const noexcept { return state_ ? state_->steps : 0; }
Submission GpuXpbdFrame::submission() const noexcept { return state_ ? state_->execution.submission() : Submission{}; }
const graph::Execution* GpuXpbdFrame::execution() const noexcept { return state_ ? &state_->execution : nullptr; }
std::string_view GpuXpbdFrame::plan_text() const noexcept { return state_ ? state_->report.text() : std::string_view{}; }
Result<bool> GpuXpbdFrame::wait(SubmissionQueue& queue,std::uint64_t timeout) const {
    if (!state_) return std::unexpected(Error{ErrorCode::invalid_state,"empty GPU XPBD frame"});
    return queue.wait(submission(),timeout);
}
Result<GpuParticles> GpuXpbdFrame::read_particles() const {
    if (!state_ || !state_->readback) return std::unexpected(Error{ErrorCode::invalid_state,"particle readback was not requested"});
    try {
        GpuParticles result; result.positions.resize(state_->particles); result.velocities.resize(state_->particles);
        check(take(state_->execution.buffer(8))->read(0,std::as_writable_bytes(std::span{result.positions})));
        check(take(state_->execution.buffer(9))->read(0,std::as_writable_bytes(std::span{result.velocities})));
        for (std::size_t i=0;i<result.positions.size();++i) {
            const auto& p=result.positions[i]; const auto& v=result.velocities[i];
            for (float x:{p.x,p.y,p.z,v.x,v.y,v.z}) if (!std::isfinite(x) || std::abs(x)>1e6f)
                return std::unexpected(Error{ErrorCode::invalid_state,"GPU XPBD diverged; stop and recreate the experiment"});
        }
        return result;
    } catch (Error& error) { return std::unexpected(std::move(error)); }
    catch (const std::bad_alloc&) { return std::unexpected(allocation_error()); }
}
}

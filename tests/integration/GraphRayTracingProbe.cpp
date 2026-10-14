#include <dk/graphics/GraphExecution.hpp>
#include <dk/graphics/ShaderCompiler.hpp>
#include <dk/memory/MemorySystem.hpp>
#include "SubmissionInternal.hpp"
#include "RayTracingInternal.hpp"
#include <atomic>
#include <cmath>
#include <cstdio>
#include <stdexcept>

using namespace dk;
using namespace dk::graphics;
using namespace dk::graphics::graph;
namespace {
void require(bool value,const char* message) { if (!value) throw std::runtime_error(message); }
template<class T> T take(Result<T> value) { if (!value) { for (const auto& c : value.error().context) std::fprintf(stderr,"%s\n",c.c_str()); throw std::runtime_error(value.error().message); } return std::move(*value); }
void check(Result<void> value) { if (!value) throw std::runtime_error(value.error().message); }
struct Diagnostics { std::atomic<unsigned> errors=0,warnings=0; };
void diagnostic(void* pointer,const Diagnostic& message) noexcept {
    auto& data=*static_cast<Diagnostics*>(pointer);
    if (message.severity==VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) ++data.errors;
    if (message.severity==VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) ++data.warnings;
    if (message.severity>=VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
        std::fprintf(stderr,"%.*s: %.*s\n",int(message.name.size()),message.name.data(),int(message.message.size()),message.message.data());
}
PFN_vkQueueSubmit2 native_submit=nullptr;
PFN_vkWaitSemaphores native_wait=nullptr;
VkResult submit_error=VK_SUCCESS;
bool timeout_once=false;
VKAPI_ATTR VkResult VKAPI_CALL submit_override(VkQueue queue,std::uint32_t count,const VkSubmitInfo2* info,VkFence fence) {
    if (submit_error!=VK_SUCCESS) return std::exchange(submit_error,VK_SUCCESS);
    return native_submit(queue,count,info,fence);
}
VKAPI_ATTR VkResult VKAPI_CALL wait_override(VkDevice device,const VkSemaphoreWaitInfo* info,std::uint64_t timeout) {
    if (std::exchange(timeout_once,false)) return VK_TIMEOUT;
    return native_wait(device,info,timeout);
}
constexpr auto bottom=AccelerationStructureKind::bottom_level,top=AccelerationStructureKind::top_level;
constexpr std::size_t upload_i=0,input_i=1,rays_i=2,output_i=3,readback_i=4,blas_i=5,dead_i=6,tlas_i=7,table_i=8;
constexpr auto input_usage=vk::BufferUsageFlagBits::eAccelerationStructureBuildInputReadOnlyKHR|vk::BufferUsageFlagBits::eShaderDeviceAddress|vk::BufferUsageFlagBits::eTransferDst;
Use read(BufferId id) { return {id,{{vk::PipelineStageFlagBits2::eCopy,vk::AccessFlagBits2::eTransferRead}}}; }
Use write(BufferId id) { return {id,{{vk::PipelineStageFlagBits2::eCopy,vk::AccessFlagBits2::eTransferWrite},0,VK_WHOLE_SIZE,{},true}}; }
PassId add(Graph& graph,std::string_view name,std::initializer_list<Use> uses) { return take(graph.add_pass({name,{uses.begin(),uses.size()}})); }
AccelerationId declare(Graph& graph,std::string_view name,AccelerationStructureKind kind,std::initializer_list<ResourceId> deps,bool initialized=false) {
    return take(graph.declare_acceleration(name,kind,{deps.begin(),deps.size()},initialized));
}
struct Program { const RayTracingPipeline* pipeline; const BindingSet* bindings; unsigned calls=0; };
Result<void> record(PassContext& pass,void* pointer) {
    auto& program=*static_cast<Program*>(pointer); ++program.calls;
    const auto name=pass.description().name;
    if (name=="upload") return pass.copy_buffer(upload_i,input_i,36);
    if (name=="BLAS") return pass.build(blas_i);
    if (name=="TLAS") return pass.build(tlas_i);
    if (name=="trace") {
        const std::array<const BindingSet*,1> sets{program.bindings};
        require(!pass.build(tlas_i),"trace pass accepted undeclared BUILD");
        require(!pass.trace(*program.pipeline,blas_i,sets,{}, {2,1,1}),"AS index accepted as SBT");
        return pass.trace(*program.pipeline,table_i,sets,{}, {2,1,1});
    }
    if (name=="readback") return pass.copy_buffer(output_i,readback_i,64);
    return std::unexpected(Error{ErrorCode::internal_error,"culled callback invoked"});
}
Result<void> fail(PassContext&,void*) { return std::unexpected(Error{ErrorCode::conflict,"injected trace callback failure"}); }
Result<void> noop(PassContext&,void*) { return {}; }
void pixels(const Buffer& buffer) {
    std::array<float,16> values{}; check(buffer.read(0,std::as_writable_bytes(std::span{values})));
    const std::array<float,16> expected{5,.25f,.5f,7, 0,0,1,1, -1,0,0,999, 0,0,0,0};
    for (std::size_t i=0; i<values.size(); ++i) require(std::isfinite(values[i]) && std::abs(values[i]-expected[i])<1e-4f,"Graph trace/readback numeric mismatch");
}
void run(memory::ResourceHandle heap,Diagnostics& diagnostics) {
    DeviceOptions options; options.validation=ValidationMode::required; options.ray_tracing=RayTracingMode::required;
    options.diagnostic_sink=diagnostic; options.diagnostic_user_data=&diagnostics;
    auto device=take(Device::create(heap,options)); auto view=take(device.share_queue(0));
    std::printf("GPU=%s validation=%d\n",device.adapter().properties.deviceName.data(),device.validation_enabled());
    native_submit=reinterpret_cast<PFN_vkQueueSubmit2>(device.device_proc("vkQueueSubmit2"));
    native_wait=reinterpret_cast<PFN_vkWaitSemaphores>(device.device_proc("vkWaitSemaphores"));
    auto queue=take(graphics::detail::SubmissionAccess::create(heap,std::move(device),2,{submit_override,nullptr,wait_override}));
    Buffer last_readback;
    std::weak_ptr<graphics::detail::AccelerationState> weak_as;
    std::weak_ptr<graphics::detail::ShaderTableState> weak_table;
    Submission final_ticket;
    {
        auto factory=queue.resources();
        std::array<ShaderModule,4> shaders;
        const std::array names{"rayMain","missMain","visibilityMiss","hitMain"};
        const std::array stages{ShaderStage::raygeneration,ShaderStage::miss,ShaderStage::miss,ShaderStage::closesthit};
        for (std::size_t i=0; i<shaders.size(); ++i) shaders[i]=take(factory.create_shader(take(compile_shader({std::filesystem::path{DK_RT_SHADER},names[i],stages[i]},heap))));
        const std::array<const ShaderModule*,4> entries{&shaders[0],&shaders[1],&shaders[2],&shaders[3]};
        auto layout=take(factory.create_pipeline_layout(entries));
        const std::array<const ShaderModule*,2> misses{&shaders[1],&shaders[2]};
        const std::array hits{RayTracingHitGroup{&shaders[3]},RayTracingHitGroup{&shaders[3]}};
        auto pipeline=take(factory.create_ray_tracing_pipeline({&layout,&shaders[0],misses,hits,2}));
        auto table=take(factory.create_shader_binding_table(pipeline));
        const std::array<float,9> positions{-1,-1,0,1,-1,0,0,1,0};
        auto upload=take(factory.create_buffer({36,vk::BufferUsageFlagBits::eTransferSrc,BufferMemory::upload}));
        check(upload.write(0,std::as_bytes(std::span{positions})));
        auto input=take(factory.create_buffer({36,input_usage}));
        TriangleGeometry geometry{}; geometry.vertices=&input; geometry.vertex_count=3; geometry.primitive_count=1;
        auto blas=take(factory.create_blas(std::span{&geometry,1}));
        auto unused=take(factory.create_blas(std::span{&geometry,1}));
        AccelerationStructureInstance instance{}; instance.blas=&blas; instance.custom_index=7;
        auto tlas=take(factory.create_tlas(std::span{&instance,1}));
        const std::array<float,24> ray_values{0,0,5,0, 0,0,-1,10, 255,0,0,0, 8,0,5,0, 0,0,-1,10, 255,0,0,0};
        auto rays=take(factory.create_buffer({sizeof(ray_values),vk::BufferUsageFlagBits::eStorageBuffer,BufferMemory::upload}));
        check(rays.write(0,std::as_bytes(std::span{ray_values})));
        auto output=take(factory.create_buffer({64,vk::BufferUsageFlagBits::eStorageBuffer|vk::BufferUsageFlagBits::eTransferSrc}));
        auto readback=take(factory.create_buffer({64,vk::BufferUsageFlagBits::eTransferDst,BufferMemory::readback}));
        const std::array writes{BindingWrite{0,0,AccelerationStructureBinding{&tlas}},BindingWrite{1,0,BufferBinding{&rays}},BindingWrite{2,0,BufferBinding{&output}}};
        auto bindings=take(factory.create_bindings(layout,0,writes));
        auto graph=take(Graph::create(heap));
        const auto u=take(graph.declare_buffer("upload",upload.description(),Lifetime::external,true));
        const auto v=take(graph.declare_buffer("geometry",input.description(),Lifetime::external));
        const auto r=take(graph.declare_buffer("rays",rays.description(),Lifetime::external,true));
        const auto o=take(graph.declare_buffer("output",output.description(),Lifetime::external));
        const auto d=take(graph.declare_buffer("readback",readback.description(),Lifetime::external));
        const auto b=declare(graph,"blas",bottom,{v}),dead=declare(graph,"dead",bottom,{v}),t=declare(graph,"tlas",top,{b});
        const auto s=take(graph.declare_shader_table("table"));
        add(graph,"upload",{read(u),write(v)}); add(graph,"dead",{build_input(v),acceleration_build(dead)});
        add(graph,"BLAS",{build_input(v),acceleration_build(b)}); add(graph,"TLAS",{acceleration_read(b,false),acceleration_build(t)});
        add(graph,"trace",{acceleration_read(b),acceleration_read(t),shader_table_read(s),
            {r,{{vk::PipelineStageFlagBits2::eRayTracingShaderKHR,vk::AccessFlagBits2::eShaderRead}}},
            {o,{{vk::PipelineStageFlagBits2::eRayTracingShaderKHR,vk::AccessFlagBits2::eShaderWrite},0,VK_WHOLE_SIZE,{},true}}});
        add(graph,"readback",{read(o),write(d)}); check(graph.mark_output(d)); check(graph.mark_output(t)); check(graph.mark_output(s));
        auto plan=take(graph.compile()); require(plan.order().size()==5 && !plan.resources()[dead_i].retained,"unused AS build not culled");
        Program program{&pipeline,&bindings};
        std::array<PassCallback,6> callbacks{}; for (std::size_t i=0;i<callbacks.size();++i) callbacks[i]={i,record,&program};
        std::array external{ExternalBinding{upload_i,&upload},ExternalBinding{input_i,&input},ExternalBinding{rays_i,&rays},ExternalBinding{output_i,&output},ExternalBinding{readback_i,&readback},
            ExternalBinding{blas_i,nullptr,nullptr,{},&blas},ExternalBinding{tlas_i,nullptr,nullptr,{},&tlas},ExternalBinding{table_i,nullptr,nullptr,{},nullptr,&table}};
        const std::array final{FinalAccess{readback_i,{{vk::PipelineStageFlagBits2::eHost,vk::AccessFlagBits2::eHostRead}}}};
        const auto candidate=take(tlas.state());
        auto wrong_input=take(factory.create_buffer(input.description()));
        auto wrong_external=external; wrong_external[1].buffer=&wrong_input;
        require(!execute(plan,queue,{wrong_external,callbacks,final,true}) && program.calls==0,"AS dependency identity mismatch reached callbacks");
        wrong_external=external; wrong_external[6].acceleration=&blas;
        require(!execute(plan,queue,{wrong_external,callbacks,final,true}),"wrong AS kind accepted");
        wrong_external=external; wrong_external[5].acceleration=&unused;
        require(!execute(plan,queue,{wrong_external,callbacks,final,true}),"TLAS child identity mismatch accepted");
        wrong_external=external; wrong_external[5].expected_generation=99;
        require(!execute(plan,queue,{wrong_external,callbacks,final,true}),"stale AS generation accepted");
        { auto foreign=take(SubmissionQueue::create(heap,take(view.share_queue(0)),1));
          require(!execute(plan,foreign,{external,callbacks,final,true}),"foreign queue imports accepted"); check(foreign.close()); }
        auto failed_callbacks=callbacks; failed_callbacks[2].record=noop;
        require(!execute(plan,queue,{external,failed_callbacks,final,true}),"unconsumed AS BUILD accepted");
        failed_callbacks=callbacks; failed_callbacks[4].record=fail;
        require(!execute(plan,queue,{external,failed_callbacks,final,true}),"callback failure ignored");
        require(take(tlas.state()).generation==candidate.generation && !take(input.state()).initialized,"failed graph published state");
        submit_error=VK_ERROR_OUT_OF_HOST_MEMORY;
        require(!execute(plan,queue,{external,callbacks,final,true}),"submit failure ignored");
        require(take(tlas.state()).generation==0 && !take(blas.state()).access.initialized && !take(readback.state()).initialized,"failed submit published AS/readback");
        auto execution=take(execute(plan,queue,{external,callbacks,final,true}));
        require(take(queue.wait(execution.submission())),"graph completion failed"); pixels(readback);
        require(take(unused.state()).status==AccelerationStructureStatus::candidate,"culled AS was built");
        {
            auto batch=take(queue.begin()); check(batch.prepare_build(unused));
            require(!batch.finish_pass() && !queue.submit(std::move(batch)),"unconsumed scratch/BUILD preparation submitted");
        }
        require(take(unused.state()).status==AccelerationStructureStatus::candidate,"abandoned preparation published AS");
        const auto exported=std::ranges::find(execution.states(),tlas_i,&ExportedState::resource); require(exported!=execution.states().end(),"AS export absent");
        require(exported->generation==execution.submission().value() && exported->state==take(tlas.state()).access,"AS export snapshot mismatch");
        const auto original_export=*exported;
        bool build_trace=false,trace_copy=false,upload_build=false;
        for (const auto& sync : execution.synchronization()) {
            build_trace |= sync.resource==tlas_i && sync.before.access==vk::AccessFlagBits2::eAccelerationStructureWriteKHR && sync.target.stages==vk::PipelineStageFlagBits2::eRayTracingShaderKHR;
            trace_copy |= sync.resource==output_i && sync.before.stages==vk::PipelineStageFlagBits2::eRayTracingShaderKHR && sync.target.access==vk::AccessFlagBits2::eTransferRead;
            upload_build |= sync.resource==input_i && sync.before.access==vk::AccessFlagBits2::eTransferWrite && sync.target.stages==vk::PipelineStageFlagBits2::eAccelerationStructureBuildKHR;
        }
        require(build_trace && trace_copy && upload_build,"Graph synchronization report omitted RT dependency");
        // A second graph imports only immutable AS dependencies; old geometry need not survive completion.
        auto imported=take(Graph::create(heap));
        const auto ib=declare(imported,"blas",bottom,{},true),it=declare(imported,"tlas",top,{ib},true);
        const auto is=take(imported.declare_shader_table("table"));
        check(imported.mark_output(it)); check(imported.mark_output(is));
        auto imported_plan=take(imported.compile());
        for (const bool tables : {false,true}) {
            auto alias=take(Graph::create(heap));
            if (tables) { const auto a=take(alias.declare_shader_table("a")),z=take(alias.declare_shader_table("z")); check(alias.mark_output(a)); check(alias.mark_output(z)); }
            else { const auto a=declare(alias,"a",bottom,{},true),z=declare(alias,"z",bottom,{},true); check(alias.mark_output(a)); check(alias.mark_output(z)); }
            auto alias_plan=take(alias.compile());
            const std::array alias_bindings{ExternalBinding{0,nullptr,nullptr,{},tables ? nullptr : &blas,tables ? &table : nullptr},
                ExternalBinding{1,nullptr,nullptr,{},tables ? nullptr : &blas,tables ? &table : nullptr}};
            require(!execute(alias_plan,queue,{alias_bindings,{},{}}),"physical AS/SBT alias accepted");
        }
        const auto before=take(tlas.state());
        const std::array expected{before.access};
        std::array imports{ExternalBinding{0,nullptr,nullptr,{},&blas},ExternalBinding{1,nullptr,nullptr,expected,&tlas,nullptr,before.generation},ExternalBinding{2,nullptr,nullptr,{},nullptr,&table}};
        { auto foreign=take(SubmissionQueue::create(heap,take(view.share_queue(0)),1));
          require(!execute(imported_plan,foreign,{imports,{},{}}),"AS-only foreign queue import accepted"); check(foreign.close()); }
        const std::array imported_final{FinalAccess{1,acceleration_read(it,false).access}};
        auto adopted=take(execute(imported_plan,queue,{imports,{},imported_final,true})); require(take(queue.wait(adopted.submission())),"AS final import failed");
        require(take(adopted.acceleration(1))->handle()==tlas.handle() && take(adopted.shader_table(2))->regions().hit.deviceAddress==table.regions().hit.deviceAddress,"AS/SBT output ownership mismatch");
        require(!execute(imported_plan,queue,{imports,{},imported_final,true}),"stale expected AS access accepted");
        require(exported->state==original_export.state && exported->generation==original_export.generation,"old AS export mutated");
        // A fresh graph provides the same indices for read-only trace execution.
        auto repeat=take(Graph::create(heap));
        take(repeat.declare_buffer("unused upload",upload.description(),Lifetime::external,true));
        take(repeat.declare_buffer("unused input",input.description(),Lifetime::external,true));
        const auto rr=take(repeat.declare_buffer("rays",rays.description(),Lifetime::external,true));
        const auto ro=take(repeat.declare_buffer("output",output.description(),Lifetime::external,true));
        take(repeat.declare_buffer("readback",readback.description(),Lifetime::external,true));
        const auto rb=declare(repeat,"blas",bottom,{},true); declare(repeat,"unused",bottom,{},true);
        const auto rt=declare(repeat,"tlas",top,{rb},true); const auto rs=take(repeat.declare_shader_table("table"));
        // A prior pass cannot authorize undeclared AS reads in this pass.
        const auto prior_reads=add(repeat,"prior AS reads",{acceleration_read(rb),acceleration_read(rt)});
        const auto undeclared_trace=add(repeat,"trace",{shader_table_read(rs),
            {rr,{{vk::PipelineStageFlagBits2::eRayTracingShaderKHR,vk::AccessFlagBits2::eShaderRead}}},
            {ro,{{vk::PipelineStageFlagBits2::eRayTracingShaderKHR,vk::AccessFlagBits2::eShaderWrite},0,VK_WHOLE_SIZE,{},true}}});
        check(repeat.add_dependency(prior_reads,undeclared_trace));
        check(repeat.mark_output(ro));
        auto undeclared_plan=take(repeat.compile());
        const std::array undeclared_callbacks{PassCallback{0,noop},PassCallback{1,record,&program}};
        const std::array undeclared_bindings{external[2],external[3],external[5],external[6],external[7]};
        require(!execute(undeclared_plan,queue,{undeclared_bindings,undeclared_callbacks,{}}),"undeclared AS descriptors accepted");
        check(repeat.reset());
        take(repeat.declare_buffer("unused upload",upload.description(),Lifetime::external,true));
        take(repeat.declare_buffer("unused input",input.description(),Lifetime::external,true));
        const auto rr2=take(repeat.declare_buffer("rays",rays.description(),Lifetime::external,true));
        const auto ro2=take(repeat.declare_buffer("output",output.description(),Lifetime::external,true));
        const auto rd2=take(repeat.declare_buffer("readback",readback.description(),Lifetime::external,true));
        const auto rb2=declare(repeat,"blas",bottom,{},true); declare(repeat,"unused",bottom,{},true);
        const auto rt2=declare(repeat,"tlas",top,{rb2},true); const auto rs2=take(repeat.declare_shader_table("table"));
        add(repeat,"trace",{acceleration_read(rb2),acceleration_read(rt2),shader_table_read(rs2),
            {rr2,{{vk::PipelineStageFlagBits2::eRayTracingShaderKHR,vk::AccessFlagBits2::eShaderRead}}},
            {ro2,{{vk::PipelineStageFlagBits2::eRayTracingShaderKHR,vk::AccessFlagBits2::eShaderWrite},0,VK_WHOLE_SIZE,{},true}}});
        add(repeat,"readback",{read(ro2),write(rd2)}); check(repeat.mark_output(rd2));
        auto repeat_plan=take(repeat.compile());
        const std::array repeat_callbacks{PassCallback{0,record,&program},PassCallback{1,record,&program}};
        auto repeated=take(execute(repeat_plan,queue,{std::span{external}.subspan(2),repeat_callbacks,final,true})); final_ticket=repeated.submission();
        last_readback=readback.share(); weak_as=graphics::detail::ObjectAccess::state(tlas); weak_table=graphics::detail::ObjectAccess::state(table);
    }
    timeout_once=true; require(!take(queue.wait(final_ticket,0)) && !weak_as.expired() && !weak_table.expired(),"pending graph RT owners released early");
    check(queue.close()); pixels(last_readback); last_readback={};
    require(weak_as.expired() && weak_table.expired(),"closed graph retained RT owners");
    VmaTotalStatistics statistics{}; vmaCalculateStatistics(view.allocator(),&statistics); require(statistics.total.statistics.allocationCount==0,"Graph RT VMA leak");
    std::puts("upload->BLAS->TLAS->trace->readback=pass culling/import/export/identity/failures/pending/close=pass");
}
}
int main() {
    try {
        auto system=memory::MemorySystem::create().value(); auto heap=system.create_heap({"graph-rt-probe",memory::DomainCategory::render}).value();
        Diagnostics diagnostics; run(heap,diagnostics);
        std::printf("errors=%u warnings=%u liveAllocations=%zu\n",diagnostics.errors.load(),diagnostics.warnings.load(),heap.snapshot().live_allocations);
        require(!diagnostics.errors && !diagnostics.warnings && !heap.snapshot().live_allocations,"Graph RT diagnostics or Memory leak");
        heap.begin_close(); require(system.try_close().closed(),"Memory close failed"); return 0;
    } catch (const std::exception& error) { std::fprintf(stderr,"required Graph RT probe: %s\n",error.what()); return 1; }
}

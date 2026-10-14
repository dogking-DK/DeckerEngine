#include <dk/graphics/RayTracing.hpp>
#include <dk/graphics/ShaderCompiler.hpp>
#include <dk/graphics/Transfer.hpp>
#include <dk/memory/MemorySystem.hpp>
#include "RayTracingInternal.hpp"
#include "SubmissionInternal.hpp"
#include <atomic>
#include <cmath>
#include <cstdio>
#include <stdexcept>

using namespace dk;
using namespace dk::graphics;
namespace {
void require(bool value,const char* message) { if (!value) throw std::runtime_error(message); }
template<class T> T take(Result<T> value) { if (!value) throw std::runtime_error(value.error().message); return std::move(*value); }
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
using Float4=std::array<float,4>;
struct Ray { Float4 origin,direction,options; };
constexpr std::array rays{
    Ray{{0,0,5,0},{0,0,-1,10},{1,0,0,0}}, // geometry 0, primitive 0
    Ray{{4,0,5,0},{0,0,-1,10},{1,0,0,0}}, // geometry 1 selects a different SBT hit group
    Ray{{0,3,5,0},{0,0,-1,10},{1,0,0,0}}, // edge of primitive 1; replaced below by unambiguous miss
    Ray{{8,0,5,0},{0,0,-1,10},{2,0,0,0}}, // scaled/translated instance, SBT offset 4
    Ray{{8,0,5,0},{0,0,-1,10},{1,0,0,0}}, // mask rejection
    Ray{{16,0,5,0},{0,0,-1,10},{4,0,0,0}}, // any-hit IgnoreHit
    Ray{{24,0,5,0},{0,0,-1,10},{8,0,0,0}}, // same custom index, force opaque
    Ray{{0,0,5,0},{0,0,-1,4},{1,0,0,0}}, // finite TMax
    Ray{{0,0,-5,0},{0,0,1,10},{1,0,0,0}}, // back face
    Ray{{0,0,5,6},{0,0,-1,10},{1,0,0,0}}, // TMin excludes triangle
    Ray{{0,4,5,0},{0,0,-1,10},{1,0,0,0}}  // primitive 1
};
void validate_hits(const std::array<Float4,rays.size()*2>& output) {
    struct Expected { float distance,instance,primitive,geometry,hit; };
    const std::array expected{
        Expected{5,7,0,0,1},Expected{5,7,0,1,2},Expected{-1,999,0,0,0},Expected{7,8,0,0,3},
        Expected{-1,999,0,0,0},Expected{-1,999,0,0,0},Expected{5,9,0,0,1},Expected{-1,999,0,0,0},
        Expected{5,7,0,0,1},Expected{-1,999,0,0,0},Expected{5,7,1,0,1}};
    const auto near=[](float a,float b) { return std::isfinite(a) && std::abs(a-b)<0.0001f; };
    for (std::size_t i=0; i<expected.size(); ++i) {
        const auto& hit=output[i*2]; const auto& metadata=output[i*2+1]; const auto& e=expected[i];
        if (!near(hit[0],e.distance) || !near(hit[3],e.instance) || !near(metadata[0],e.primitive) || !near(metadata[1],e.geometry) ||
            !near(metadata[2],e.hit) || !near(metadata[3],e.hit ? 1.0f : 0.0f) || (e.hit && (!near(hit[1],0.25f) || !near(hit[2],0.5f)))) {
            std::fprintf(stderr,"ray %zu actual=(%g,%g,%g,%g) meta=(%g,%g,%g,%g)\n",i,hit[0],hit[1],hit[2],hit[3],metadata[0],metadata[1],metadata[2],metadata[3]);
            throw std::runtime_error("known ray result mismatch");
        }
    }
}
void run(memory::ResourceHandle heap,Diagnostics& diagnostics) {
    DeviceOptions options; options.validation=ValidationMode::required; options.ray_tracing=RayTracingMode::required;
    options.diagnostic_sink=diagnostic; options.diagnostic_user_data=&diagnostics;
    auto device=take(Device::create(heap,options)); auto view=take(device.share_queue(0));
    std::printf("GPU=%s validation=%d handle=%u align=%u base=%u\n",device.adapter().properties.deviceName.data(),device.validation_enabled(),
        device.adapter().ray_tracing.pipeline_limits.shaderGroupHandleSize,device.adapter().ray_tracing.pipeline_limits.shaderGroupHandleAlignment,
        device.adapter().ray_tracing.pipeline_limits.shaderGroupBaseAlignment);
    native_submit=reinterpret_cast<PFN_vkQueueSubmit2>(device.device_proc("vkQueueSubmit2"));
    native_wait=reinterpret_cast<PFN_vkWaitSemaphores>(device.device_proc("vkWaitSemaphores"));
    auto queue=take(graphics::detail::SubmissionAccess::create(heap,std::move(device),2,{submit_override,nullptr,wait_override}));
    auto factory=queue.resources();
    const auto source=std::filesystem::path{DK_RT_SHADER};
    {
        auto disabled_options=options; disabled_options.ray_tracing=RayTracingMode::disabled;
        auto disabled=take(SubmissionQueue::create(heap,take(Device::create(heap,disabled_options)),1));
        const auto artifact=take(compile_shader({source,"rayMain",ShaderStage::raygeneration},heap));
        require(!disabled.resources().create_shader(artifact),"disabled RT device accepted RT shader");
        require(!disabled.resources().create_ray_tracing_pipeline({}),"disabled RT device accepted pipeline");
        { auto batch=take(disabled.begin()); require(!batch.ray_tracing(),"disabled RT device accepted trace encoder"); }
        check(disabled.close());
    }
    std::array<ShaderModule,7> shaders;
    const std::array names{"rayMain","missMain","visibilityMiss","hitMain","anyMain","hitGeometry","hitAlternate"};
    const std::array stages{ShaderStage::raygeneration,ShaderStage::miss,ShaderStage::miss,ShaderStage::closesthit,ShaderStage::anyhit,ShaderStage::closesthit,ShaderStage::closesthit};
    for (std::size_t i=0; i<shaders.size(); ++i) shaders[i]=take(factory.create_shader(take(compile_shader({source,names[i],stages[i]},heap))));
    const std::array<const ShaderModule*,7> shader_refs{&shaders[0],&shaders[1],&shaders[2],&shaders[3],&shaders[4],&shaders[5],&shaders[6]};
    auto layout=take(factory.create_pipeline_layout(shader_refs));
    const std::array<const ShaderModule*,2> misses{&shaders[1],&shaders[2]};
    std::array<RayTracingHitGroup,8> hits;
    for (auto& hit : hits) hit={&shaders[3],&shaders[4]};
    hits[2].closest_hit=&shaders[5]; hits[4].closest_hit=hits[6].closest_hit=&shaders[6];
    RayTracingPipelineDesc description{&layout,&shaders[0],misses,hits,2};
    auto pipeline=take(factory.create_ray_tracing_pipeline(description));
    auto table=take(factory.create_shader_binding_table(pipeline));
    const auto regions=table.regions(); const auto& limits=view.adapter().ray_tracing.pipeline_limits;
    require(regions.raygeneration.size==regions.raygeneration.stride && regions.raygeneration.deviceAddress%limits.shaderGroupBaseAlignment==0 &&
        regions.miss.deviceAddress%limits.shaderGroupBaseAlignment==0 && regions.hit.deviceAddress%limits.shaderGroupBaseAlignment==0 &&
        regions.hit.stride%limits.shaderGroupHandleAlignment==0 && regions.callable.size==0,"invalid exposed SBT regions");
    auto wrong=description; wrong.raygeneration=&shaders[1]; require(!factory.create_ray_tracing_pipeline(wrong),"wrong RT stage accepted");
    wrong=description; wrong.ray_type_count=1; require(!factory.create_ray_tracing_pipeline(wrong),"SBT ray type mismatch accepted");
    {
        auto incomplete_layout=take(factory.create_pipeline_layout(misses));
        wrong=description; wrong.layout=&incomplete_layout;
        require(!factory.create_ray_tracing_pipeline(wrong),"layout missing raygen stage accepted");
        auto foreign=take(SubmissionQueue::create(heap,take(view.share_queue(0)),1));
        require(!foreign.resources().create_ray_tracing_pipeline(description),"foreign queue pipeline inputs accepted");
        require(!foreign.resources().create_shader_binding_table(pipeline),"foreign queue SBT accepted");
        { auto batch=take(foreign.begin()); require(!batch.prepare_trace(table,{}),"foreign queue SBT preparation accepted"); }
        check(foreign.close());
    }
    const std::array defines{ShaderDefine{"DK_BAD_PAYLOAD","1"}};
    auto bad_shader=take(factory.create_shader(take(compile_shader({source,"missMain",ShaderStage::miss,{},defines},heap))));
    const std::array<const ShaderModule*,2> bad_misses{&bad_shader,&shaders[2]};
    wrong=description; wrong.misses=bad_misses; require(!factory.create_ray_tracing_pipeline(wrong),"same-size different payload accepted");
    graphics::detail::ObjectAccess::fail_creation(factory,VK_ERROR_OUT_OF_DEVICE_MEMORY);
    require(!factory.create_ray_tracing_pipeline(description),"pipeline creation failure ignored");
    graphics::detail::ObjectAccess::fail_creation(factory,VK_ERROR_OUT_OF_DEVICE_MEMORY);
    require(!factory.create_shader_binding_table(pipeline),"SBT creation failure ignored");
    auto other_pipeline=take(factory.create_ray_tracing_pipeline(description));
    auto other_table=take(factory.create_shader_binding_table(other_pipeline));
    constexpr std::array<float,27> positions{-1,-1,0,1,-1,0,0,1,0, -1,3,0,1,3,0,0,5,0, 3,-1,0,5,-1,0,4,1,0};
    auto input=take(factory.create_buffer({sizeof(positions),vk::BufferUsageFlagBits::eAccelerationStructureBuildInputReadOnlyKHR|vk::BufferUsageFlagBits::eShaderDeviceAddress,BufferMemory::upload}));
    check(input.write(0,std::as_bytes(std::span{positions})));
    std::array<TriangleGeometry,2> geometries{};
    geometries[0].vertices=&input; geometries[0].vertex_count=6; geometries[0].primitive_count=2; geometries[0].opaque=false;
    geometries[1]=geometries[0]; geometries[1].vertex_offset=72; geometries[1].vertex_count=3; geometries[1].primitive_count=1;
    auto blas=take(factory.create_blas(geometries));
    std::array<AccelerationStructureInstance,4> instances{};
    for (std::uint32_t i=0; i<instances.size(); ++i) {
        instances[i].blas=&blas; instances[i].custom_index=7+i; instances[i].mask=1u<<i; instances[i].transform[3]=8.0f*float(i);
    }
    instances[1].transform[0]=2; instances[1].transform[5]=0.5f; instances[1].transform[11]=-2; instances[1].sbt_record_offset=4;
    instances[3].custom_index=9; instances[3].flags=vk::GeometryInstanceFlagBitsKHR::eForceOpaque;
    auto tlas=take(factory.create_tlas(instances));
    auto bad_instance=instances[0]; bad_instance.sbt_record_offset=8;
    auto bad_tlas=take(factory.create_tlas(std::span{&bad_instance,1}));
    auto ray_data=rays; ray_data[2].origin[1]=2; // Between the two triangles.
    auto ray_buffer=take(factory.create_buffer({sizeof(ray_data),vk::BufferUsageFlagBits::eStorageBuffer,BufferMemory::upload}));
    check(ray_buffer.write(0,std::as_bytes(std::span{ray_data})));
    auto output=take(factory.create_buffer({rays.size()*2*sizeof(Float4),vk::BufferUsageFlagBits::eStorageBuffer|vk::BufferUsageFlagBits::eTransferSrc}));
    const std::array writes{BindingWrite{0,0,AccelerationStructureBinding{&tlas}},BindingWrite{1,0,BufferBinding{&ray_buffer}},BindingWrite{2,0,BufferBinding{&output}}};
    auto bindings=take(factory.create_bindings(layout,0,writes));
    auto bad_writes=writes; bad_writes[0].resource=AccelerationStructureBinding{&bad_tlas};
    auto bad_bindings=take(factory.create_bindings(layout,0,bad_writes));
    bad_writes[0].resource=AccelerationStructureBinding{&blas}; require(!factory.create_bindings(layout,0,bad_writes),"BLAS descriptor accepted");
    const std::array<const BindingSet*,1> sets{&bindings},bad_sets{&bad_bindings};
    const std::array<const AccelerationStructure*,1> structures{&tlas},bad_structures{&bad_tlas};
    std::array uses{buffer_use(ray_buffer,vk::PipelineStageFlagBits2::eRayTracingShaderKHR,vk::AccessFlagBits2::eShaderRead),
        buffer_use(output,vk::PipelineStageFlagBits2::eRayTracingShaderKHR,vk::AccessFlagBits2::eShaderWrite)};
    uses[1].full_overwrite=true;
    auto batch=take(queue.begin());
    require(!batch.prepare_trace(table,structures),"unbuilt TLAS accepted");
    check(batch.build(blas)); check(batch.build(tlas)); check(batch.build(bad_tlas));
    auto encoder=take(batch.ray_tracing());
    require(!encoder.trace(pipeline,table,sets,{}, {static_cast<std::uint32_t>(rays.size()),1,1}),"unprepared trace accepted");
    check(batch.prepare(uses)); check(batch.prepare_trace(table,structures)); check(batch.prepare_trace(other_table,bad_structures));
    require(!encoder.trace(pipeline,other_table,sets,{}, {1,1,1}),"foreign pipeline SBT accepted");
    require(!encoder.trace(pipeline,table,bad_sets,{}, {1,1,1}),"out of range instance SBT accepted");
    require(!encoder.trace(pipeline,table,sets,{}, {UINT32_MAX,2,1}),"oversized trace accepted");
    require(!encoder.trace(pipeline,table,{}, {}, {1,1,1}),"missing descriptors accepted");
    check(encoder.trace(pipeline,table,sets,{}, {static_cast<std::uint32_t>(rays.size()),1,1}));
    check(batch.finish_pass()); require(!encoder.trace(pipeline,table,sets,{}, {1,1,1}),"ended encoder accepted trace");
    auto readback=take(batch.readback(output));
    auto ticket=take(queue.submit(std::move(batch))); require(take(queue.wait(ticket)),"trace completion failed");
    std::array<Float4,rays.size()*2> result{}; require(take(readback.try_read(std::as_writable_bytes(std::span{result}))),"trace readback not ready");
    validate_hits(result); readback={};
    const auto before=take(tlas.state()); const auto output_before=take(output.state());
    auto failed=take(queue.begin()); check(failed.prepare(uses)); check(failed.prepare_trace(table,structures));
    auto failed_encoder=take(failed.ray_tracing()); check(failed_encoder.trace(pipeline,table,sets,{}, {static_cast<std::uint32_t>(rays.size()),1,1}));
    submit_error=VK_ERROR_OUT_OF_HOST_MEMORY; require(!queue.submit(std::move(failed)),"failed trace submission accepted");
    require(take(tlas.state()).generation==before.generation && take(tlas.state()).access==before.access && take(output.state())==output_before,"failed trace changed committed ledgers");
    auto last=take(queue.begin()); check(last.prepare(uses)); check(last.prepare_trace(table,structures));
    auto last_encoder=take(last.ray_tracing()); check(last_encoder.trace(pipeline,table,sets,{}, {static_cast<std::uint32_t>(rays.size()),1,1}));
    readback=take(last.readback(output)); ticket=take(queue.submit(std::move(last)));
    std::weak_ptr<graphics::detail::RayTracingState> pipeline_owner=graphics::detail::ObjectAccess::state(pipeline);
    std::weak_ptr<graphics::detail::AccelerationState> blas_owner=graphics::detail::ObjectAccess::state(blas);
    pipeline={}; table={}; bindings={}; blas={}; tlas={}; input={}; output={}; ray_buffer={};
    timeout_once=true; require(!take(queue.wait(ticket,0)) && !pipeline_owner.expired() && !blas_owner.expired(),"pending RT dependencies released early");
    check(queue.close()); require(take(readback.try_read(std::as_writable_bytes(std::span{result}))),"close did not publish trace readback"); validate_hits(result);
    require(pipeline_owner.expired(),"completed trace retained pipeline unexpectedly");
    readback={}; bad_bindings={}; bad_tlas={}; other_table={}; other_pipeline={}; layout={}; bad_shader={};
    for (auto& shader : shaders) shader={};
    require(blas_owner.expired(),"AS descriptor retained BLAS after release");
    VmaTotalStatistics stats{}; vmaCalculateStatistics(view.allocator(),&stats); require(stats.total.statistics.allocationCount==0,"RT VMA allocations remained");
    std::printf("rays=%zu primary+visibility hit/barycentrics/distance/primitive/geometry/instance/mask/anyhit/SBT=pass failures/pending/close=pass\n",rays.size());
}
}
int main() {
    try {
        auto system=memory::MemorySystem::create().value(); auto heap=system.create_heap({"rt-pipeline-probe",memory::DomainCategory::render}).value();
        Diagnostics diagnostics; run(heap,diagnostics);
        std::printf("errors=%u warnings=%u liveAllocations=%zu\n",diagnostics.errors.load(),diagnostics.warnings.load(),heap.snapshot().live_allocations);
        require(diagnostics.errors==0 && diagnostics.warnings==0 && heap.snapshot().live_allocations==0,"RT validation diagnostics or leaked CPU allocations");
        heap.begin_close(); require(system.try_close().closed(),"Memory system failed to close"); return 0;
    } catch (const std::exception& error) { std::fprintf(stderr,"required RT pipeline probe: %s\n",error.what()); return 1; }
}

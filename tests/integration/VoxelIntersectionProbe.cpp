#include <dk/graphics/RayTracing.hpp>
#include <dk/graphics/ShaderCompiler.hpp>
#include <dk/graphics/Transfer.hpp>
#include <dk/graphics/GraphExecution.hpp>
#include <dk/assets/VoxelBuilder.hpp>
#include <fstream>
#include <dk/memory/MemorySystem.hpp>
#include "RayTracingInternal.hpp"
#include "SubmissionInternal.hpp"
#include <atomic>
#include <cmath>
#include <cstdio>
#include <stdexcept>

using namespace dk;
using namespace dk::graphics;
using namespace dk::graphics::graph;
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
    Ray{{0,0,5,0},{0,0,-1,10},{1,0,0,0}},
    Ray{{4,0,5,0},{0,0,-1,10},{1,0,0,0}},
    Ray{{2,0,5,0},{0,0,-1,10},{1,0,0,0}},
    Ray{{8,0,5,0},{0,0,-1,10},{2,0,0,0}},
    Ray{{8,0,5,0},{0,0,-1,10},{1,0,0,0}},
    Ray{{16,0,5,0},{0,0,-1,10},{4,0,0,0}},
    Ray{{24,0,5,0},{0,0,-1,10},{8,0,0,0}},
    Ray{{0,0,5,0},{0,0,-1,3.9f},{1,0,0,0}},
    Ray{{0,0,-5,0},{0,0,1,10},{1,0,0,0}},
    Ray{{0,0,5,4.5f},{0,0,-1,10},{1,0,0,0}},
    Ray{{0,3,5,0},{0,0,-1,10},{1,0,0,0}},
    Ray{{0,0,0,.001f},{0,0,1,10},{1,0,0,0}},
    Ray{{.9f,.9f,5,0},{0,0,-1,10},{1,0,0,0}},
    Ray{{0,0,5,5.5f},{0,0,-1,10},{1,0,0,0}},
    Ray{{4,0,5,0},{0,0,-2,10},{1,0,0,0}},
    Ray{{8,0,5,0},{0,0,-1,4},{2,0,0,0}},
    Ray{{4,0,0,.001f},{0,0,1,10},{1,0,0,0}}
};
void validate_hits(const std::array<Float4,rays.size()*2>& output) {
    struct Expected { float distance,instance,primitive,geometry,hit; };
    const std::array expected{
        Expected{4,11,0,0,4},Expected{4,11,0,1,4},Expected{-1,999,0,0,0},Expected{5,12,0,0,4},
        Expected{-1,999,0,0,0},Expected{-1,999,0,0,0},Expected{4,9,0,0,4},Expected{-1,999,0,0,0},
        Expected{4,11,0,0,4},Expected{5,7,0,0,1},Expected{4,11,1,0,4},Expected{1,11,0,0,4},
        Expected{-1,999,0,0,0},Expected{6,11,0,0,4},Expected{2,11,0,1,4},Expected{-1,999,0,0,0},Expected{1,11,0,1,4}};
    const auto near=[](float a,float b) { return std::isfinite(a) && std::abs(a-b)<0.0001f; };
    for (std::size_t i=0; i<expected.size(); ++i) {
        const auto& hit=output[i*2]; const auto& metadata=output[i*2+1]; const auto& e=expected[i];
        if (!near(hit[0],e.distance) || !near(hit[3],e.instance) || !near(metadata[0],e.primitive) || !near(metadata[1],e.geometry) ||
            !near(metadata[2],e.hit) || !near(metadata[3],e.hit ? 1.0f : 0.0f) || (e.hit && (!near(hit[1],e.hit==4 ? (e.primitive==1 ? 53.0f : 37.0f) : 0.25f) || !near(hit[2],e.hit==4 ? 19.0f : 0.5f)))) {
            std::fprintf(stderr,"ray %zu actual=(%g,%g,%g,%g) meta=(%g,%g,%g,%g)\n",i,hit[0],hit[1],hit[2],hit[3],metadata[0],metadata[1],metadata[2],metadata[3]);
            throw std::runtime_error("known ray result mismatch");
        }
    }
}
void graph_probe(memory::ResourceHandle heap,SubmissionQueue& queue,const Buffer& triangles,const Buffer& bounds,
    const Buffer& upload,const Buffer& nano,const Buffer& rays_buffer,const Buffer& output,
    const AccelerationStructure& blas,const AccelerationStructure& procedural,const AccelerationStructure& tlas,
    const ShaderBindingTable& table,const RayTracingPipeline& pipeline,const BindingSet& bindings) {
    auto readback=take(queue.resources().create_buffer({output.size(),vk::BufferUsageFlagBits::eTransferDst,BufferMemory::readback}));
    auto graph=take(Graph::create(heap));
    const auto tr=take(graph.declare_buffer("triangles",triangles.description(),Lifetime::external,true)); // 0
    const auto ab=take(graph.declare_buffer("bounds",bounds.description(),Lifetime::external,true)); // 1
    const auto up=take(graph.declare_buffer("nano upload",upload.description(),Lifetime::external,true)); // 2
    const auto na=take(graph.declare_buffer("nano",nano.description(),Lifetime::external)); // 3
    const auto ra=take(graph.declare_buffer("rays",rays_buffer.description(),Lifetime::external,true)); // 4
    const auto ou=take(graph.declare_buffer("output",output.description(),Lifetime::external)); // 5
    const auto rb=take(graph.declare_buffer("readback",readback.description(),Lifetime::external)); // 6
    const std::array<ResourceId,1> triangle_deps{tr},aabb_deps{ab};
    const auto bl=take(graph.declare_acceleration("triangle BLAS",AccelerationStructureKind::bottom_level,triangle_deps)); // 7
    const auto pr=take(graph.declare_acceleration("procedural BLAS",AccelerationStructureKind::bottom_level,aabb_deps)); // 8
    const std::array<ResourceId,2> children{bl,pr};
    const auto tl=take(graph.declare_acceleration("mixed TLAS",AccelerationStructureKind::top_level,children)); // 9
    const auto sb=take(graph.declare_shader_table("mixed SBT")); // 10
    const auto add=[&](std::string_view name,std::initializer_list<Use> uses) { return take(graph.add_pass({name,{uses.begin(),uses.size()}})); };
    add("upload",{{up,{{vk::PipelineStageFlagBits2::eCopy,vk::AccessFlagBits2::eTransferRead}}},
        {na,{{vk::PipelineStageFlagBits2::eCopy,vk::AccessFlagBits2::eTransferWrite},0,VK_WHOLE_SIZE,{},true}}});
    add("triangle",{build_input(tr),acceleration_build(bl)});
    add("procedural",{build_input(ab),acceleration_build(pr)});
    add("TLAS",{acceleration_read(bl,false),acceleration_read(pr,false),acceleration_build(tl)});
    add("trace",{acceleration_read(bl),acceleration_read(pr),acceleration_read(tl),shader_table_read(sb),
        {na,{{vk::PipelineStageFlagBits2::eRayTracingShaderKHR,vk::AccessFlagBits2::eShaderRead}}},
        {ra,{{vk::PipelineStageFlagBits2::eRayTracingShaderKHR,vk::AccessFlagBits2::eShaderRead}}},
        {ou,{{vk::PipelineStageFlagBits2::eRayTracingShaderKHR,vk::AccessFlagBits2::eShaderWrite},0,VK_WHOLE_SIZE,{},true}}});
    add("readback",{{ou,{{vk::PipelineStageFlagBits2::eCopy,vk::AccessFlagBits2::eTransferRead}}},
        {rb,{{vk::PipelineStageFlagBits2::eCopy,vk::AccessFlagBits2::eTransferWrite},0,VK_WHOLE_SIZE,{},true}}});
    check(graph.mark_output(rb));
    auto plan=take(graph.compile());
    struct Context { const RayTracingPipeline* pipeline; const BindingSet* bindings; vk::DeviceSize nano_size,output_size; bool fail=false; } context{&pipeline,&bindings,nano.size(),output.size()};
    const auto record=+[](PassContext& pass,void* pointer)->Result<void> {
        const auto& c=*static_cast<Context*>(pointer);
        switch(pass.pass_index()) {
        case 0: return pass.copy_buffer(2,3,c.nano_size);
        case 1: return pass.build(7);
        case 2: return pass.build(8);
        case 3: return pass.build(9);
        case 4: {
            if (c.fail) return std::unexpected(Error{ErrorCode::conflict,"injected procedural callback failure"});
            const std::array<const BindingSet*,1> sets{c.bindings};
            return pass.trace(*c.pipeline,10,sets,{}, {static_cast<std::uint32_t>(rays.size()),1,1});
        }
        case 5: return pass.copy_buffer(5,6,c.output_size);
        default: return std::unexpected(Error{ErrorCode::internal_error,"unexpected pass"});
        }
    };
    std::array<PassCallback,6> callbacks{}; for(std::size_t i=0;i<6;++i) callbacks[i]={i,record,&context};
    const std::array external{ExternalBinding{0,&triangles},ExternalBinding{1,&bounds},ExternalBinding{2,&upload},ExternalBinding{3,&nano},
        ExternalBinding{4,&rays_buffer},ExternalBinding{5,&output},ExternalBinding{6,&readback},
        ExternalBinding{7,nullptr,nullptr,{},&blas},ExternalBinding{8,nullptr,nullptr,{},&procedural},ExternalBinding{9,nullptr,nullptr,{},&tlas},
        ExternalBinding{10,nullptr,nullptr,{},nullptr,&table}};
    const std::array final{FinalAccess{6,{{vk::PipelineStageFlagBits2::eHost,vk::AccessFlagBits2::eHostRead}}}};
    context.fail=true; require(!execute(plan,queue,{external,callbacks,final,true}),"callback failure ignored"); context.fail=false;
    submit_error=VK_ERROR_OUT_OF_HOST_MEMORY; require(!execute(plan,queue,{external,callbacks,final,true}),"graph submit failure ignored");
    require(take(procedural.state()).generation==0 && !take(tlas.state()).access.initialized && !take(nano.state()).initialized,
        "failed graph committed AABB/TLAS/Nano state");
    auto execution=take(execute(plan,queue,{external,callbacks,final,true}));
    require(take(queue.wait(execution.submission())),"mixed Graph trace timeout");
    std::array<Float4,rays.size()*2> result{}; check(readback.read(0,std::as_writable_bytes(std::span{result}))); validate_hits(result);
    bool transfer_intersection=false,build_trace=false;
    for(const auto& sync:execution.synchronization()) {
        transfer_intersection|=sync.resource==3 && sync.before.access==vk::AccessFlagBits2::eTransferWrite && sync.target.stages==vk::PipelineStageFlagBits2::eRayTracingShaderKHR;
        build_trace|=sync.resource==9 && sync.before.access==vk::AccessFlagBits2::eAccelerationStructureWriteKHR && sync.target.stages==vk::PipelineStageFlagBits2::eRayTracingShaderKHR;
    }
    require(transfer_intersection && build_trace,"missing Nano transfer/intersection or AS build/trace synchronization");
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
    const std::array includes{std::filesystem::path{DK_VOXEL_SHADER_DIR},std::filesystem::path{DK_VOXEL_NANO_INCLUDE_DIR}};
    const std::array nano_defines{ShaderDefine{"DK_NANOVDB","1"}};
    {
        auto disabled_options=options; disabled_options.ray_tracing=RayTracingMode::disabled;
        auto disabled=take(SubmissionQueue::create(heap,take(Device::create(heap,disabled_options)),1));
        const auto artifact=take(compile_shader({source,"rayMain",ShaderStage::raygeneration},heap));
        require(!disabled.resources().create_shader(artifact),"disabled RT device accepted RT shader");
        const auto intersection=take(compile_shader({source,"intersectionMain",ShaderStage::intersection,includes,nano_defines},heap));
        require(!disabled.resources().create_shader(intersection),"disabled RT device accepted intersection shader");
        require(!disabled.resources().create_aabb_blas({}),"disabled RT accepted AABB");
        require(!disabled.resources().create_ray_tracing_pipeline({}),"disabled RT device accepted pipeline");
        { auto batch=take(disabled.begin()); require(!batch.ray_tracing(),"disabled RT device accepted trace encoder"); }
        check(disabled.close());
    }
    std::array<ShaderModule,10> shaders;
    const std::array names{"rayMain","missMain","visibilityMiss","hitMain","anyMain","hitGeometry","hitAlternate","intersectionMain","proceduralHit","proceduralAny"};
    const std::array stages{ShaderStage::raygeneration,ShaderStage::miss,ShaderStage::miss,ShaderStage::closesthit,ShaderStage::anyhit,ShaderStage::closesthit,ShaderStage::closesthit,ShaderStage::intersection,ShaderStage::closesthit,ShaderStage::anyhit};
    for (std::size_t i=0; i<shaders.size(); ++i) shaders[i]=take(factory.create_shader(take(compile_shader({source,names[i],stages[i],includes,nano_defines},heap))));
    const std::array<const ShaderModule*,10> shader_refs{&shaders[0],&shaders[1],&shaders[2],&shaders[3],&shaders[4],&shaders[5],&shaders[6],&shaders[7],&shaders[8],&shaders[9]};
    auto layout=take(factory.create_pipeline_layout(shader_refs));
    const std::array<const ShaderModule*,2> misses{&shaders[1],&shaders[2]};
    std::array<RayTracingHitGroup,8> hits;
    for (auto& hit : hits) hit={&shaders[3],&shaders[4]};
    for (std::size_t i=4;i<8;++i) hits[i]={&shaders[8],&shaders[9],RayGeometryKind::procedural,&shaders[7]};
    RayTracingPipelineDesc description{&layout,&shaders[0],misses,hits,2};
    {
        auto rejected=hits; rejected[4].intersection=nullptr;
        auto bad=description; bad.hits=rejected; require(!factory.create_ray_tracing_pipeline(bad),"missing intersection accepted");
        rejected=hits; rejected[0].intersection=&shaders[7]; require(!factory.create_ray_tracing_pipeline(bad),"triangle intersection accepted");
        const std::array bad_defines{ShaderDefine{"DK_BAD_ATTRIBUTE","1"},ShaderDefine{"DK_NANOVDB","1"}};
        auto mismatch=take(factory.create_shader(take(compile_shader({source,"proceduralHit",ShaderStage::closesthit,includes,bad_defines},heap))));
        rejected=hits; rejected[4].closest_hit=&mismatch; require(!factory.create_ray_tracing_pipeline(bad),"same-size wrong attribute type accepted");
        const std::array<const ShaderModule*,5> no_intersection{&shaders[0],&shaders[1],&shaders[2],&shaders[8],&shaders[9]};
        auto incomplete=take(factory.create_pipeline_layout(no_intersection));
        bad=description; bad.layout=&incomplete; require(!factory.create_ray_tracing_pipeline(bad),"missing intersection descriptor visibility accepted");
    }
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
    const std::array boxes{vk::AabbPositionsKHR{-1,-1,-1,1,1,1},vk::AabbPositionsKHR{-1,2,-1,1,4,1},vk::AabbPositionsKHR{3,-1,-1,5,1,1}};
    auto bounds=take(factory.create_aabb_input(boxes));
    auto bad_bounds=boxes; bad_bounds[0].maxX=-2; require(!factory.create_aabb_input(bad_bounds),"inverted bounds accepted");
    std::array<AabbGeometry,2> aabbs{AabbGeometry{&bounds,0,24,2,false},AabbGeometry{&bounds,48,24,1,false}};
    auto procedural=take(factory.create_aabb_blas(aabbs));
    auto bad_aabb=aabbs[0]; bad_aabb.offset=4; require(!factory.create_aabb_blas(std::span{&bad_aabb,1}),"misaligned AABB accepted");
    bad_aabb=aabbs[0]; bad_aabb.primitive_count=4; require(!factory.create_aabb_blas(std::span{&bad_aabb,1}),"AABB range accepted");
    std::array<AccelerationStructureInstance,5> instances{};
    instances[0].blas=&blas; instances[0].custom_index=7; instances[0].mask=1;
    for (std::uint32_t i=1;i<instances.size();++i) {
        instances[i].blas=&procedural; instances[i].custom_index=i+10; instances[i].mask=1u<<(i-1);
        instances[i].transform[3]=8.0f*float(i-1); instances[i].sbt_record_offset=4;
    }
    instances[2].transform[0]=2; instances[2].transform[5]=0.5f; instances[2].transform[10]=2; instances[2].transform[11]=-2;
    instances[3].custom_index=instances[4].custom_index=9; instances[4].flags=vk::GeometryInstanceFlagBitsKHR::eForceOpaque;
    auto tlas=take(factory.create_tlas(instances));
    auto bad_instance=instances[1]; bad_instance.sbt_record_offset=0; // In-range triangle records for AABB must be rejected.
    auto bad_tlas=take(factory.create_tlas(std::span{&bad_instance,1}));
    auto ray_data=rays;
    auto ray_buffer=take(factory.create_buffer({sizeof(ray_data),vk::BufferUsageFlagBits::eStorageBuffer,BufferMemory::upload}));
    check(ray_buffer.write(0,std::as_bytes(std::span{ray_data})));
    auto output=take(factory.create_buffer({rays.size()*2*sizeof(Float4),vk::BufferUsageFlagBits::eStorageBuffer|vk::BufferUsageFlagBits::eTransferSrc}));
    assets::VoxelBuildOptions voxel_options; voxel_options.palette_size=64;
    const std::array blocks{assets::VoxelBlock{{7,8,9},1,37,true},assets::VoxelBlock{{-128,-128,-128},128,53,false}};
    auto grid=take(assets::build_voxel_grid(blocks,voxel_options));
    auto nano_upload=take(factory.create_buffer({grid.bytes().size(),vk::BufferUsageFlagBits::eTransferSrc,BufferMemory::upload}));
    check(nano_upload.write(0,grid.bytes()));
    auto nano=take(factory.create_buffer({grid.bytes().size(),vk::BufferUsageFlagBits::eStorageBuffer|vk::BufferUsageFlagBits::eTransferDst}));
    const std::array writes{BindingWrite{0,0,AccelerationStructureBinding{&tlas}},BindingWrite{1,0,BufferBinding{&ray_buffer}},BindingWrite{2,0,BufferBinding{&output}},BindingWrite{3,0,BufferBinding{&nano}}};
    auto bindings=take(factory.create_bindings(layout,0,writes));
    auto bad_writes=writes; bad_writes[0].resource=AccelerationStructureBinding{&bad_tlas};
    auto bad_bindings=take(factory.create_bindings(layout,0,bad_writes));
    bad_writes[0].resource=AccelerationStructureBinding{&blas}; require(!factory.create_bindings(layout,0,bad_writes),"BLAS descriptor accepted");
    const std::array<const BindingSet*,1> sets{&bindings},bad_sets{&bad_bindings};
    const std::array<const AccelerationStructure*,1> structures{&tlas},bad_structures{&bad_tlas};
    std::array uses{buffer_use(ray_buffer,vk::PipelineStageFlagBits2::eRayTracingShaderKHR,vk::AccessFlagBits2::eShaderRead),
        buffer_use(output,vk::PipelineStageFlagBits2::eRayTracingShaderKHR,vk::AccessFlagBits2::eShaderWrite),
        buffer_use(nano,vk::PipelineStageFlagBits2::eRayTracingShaderKHR,vk::AccessFlagBits2::eShaderRead)};
    uses[1].full_overwrite=true;
    { auto empty=take(queue.begin()); require(!empty.prepare_trace(table,structures),"unbuilt TLAS accepted"); }
    graph_probe(heap,queue,input,bounds,nano_upload,nano,ray_buffer,output,blas,procedural,tlas,table,pipeline,bindings);
    std::weak_ptr<graphics::detail::ResourceState> bounds_owner=graphics::detail::ObjectAccess::state(bounds);
    bounds={}; require(bounds_owner.expired(),"completed AABB build retained input buffer");
    auto batch=take(queue.begin()); check(batch.build(bad_tlas));
    auto encoder=take(batch.ray_tracing());
    require(!encoder.trace(pipeline,table,sets,{}, {static_cast<std::uint32_t>(rays.size()),1,1}),"unprepared trace accepted");
    check(batch.prepare(uses)); check(batch.prepare_trace(table,structures)); check(batch.prepare_trace(other_table,bad_structures));
    require(!encoder.trace(pipeline,other_table,sets,{}, {1,1,1}),"foreign pipeline SBT accepted");
    require(!encoder.trace(pipeline,table,bad_sets,{}, {1,1,1}),"in-range wrong geometry kind SBT accepted");
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
    std::weak_ptr<graphics::detail::ResourceState> nano_owner=graphics::detail::ObjectAccess::state(nano);
    pipeline={}; table={}; bindings={}; blas={}; tlas={}; input={}; output={}; ray_buffer={}; procedural={}; bounds={}; nano={}; nano_upload={};
    bad_bindings={}; bad_tlas={};
    timeout_once=true; require(!take(queue.wait(ticket,0)) && !pipeline_owner.expired() && !blas_owner.expired() && !nano_owner.expired(),"pending RT dependencies released early");
    check(queue.close()); require(take(readback.try_read(std::as_writable_bytes(std::span{result}))),"close did not publish trace readback"); validate_hits(result);
    require(nano_owner.expired(),"completed trace retained NanoVDB unexpectedly");
    require(pipeline_owner.expired(),"completed trace retained pipeline unexpectedly");
    readback={}; bad_bindings={}; bad_tlas={}; other_table={}; other_pipeline={}; layout={}; bad_shader={};
    for (auto& shader : shaders) shader={};
    require(blas_owner.expired(),"AS descriptor retained BLAS after release");
    VmaTotalStatistics stats{}; vmaCalculateStatistics(view.allocator(),&stats); require(stats.total.statistics.allocationCount==0,"RT VMA allocations remained");
    std::printf("rays=%zu radiance+visibility mixed SBT/PNanoVDB leaf+inactive tile/attributes/intervals/inside/Graph=pass failures/build-input release/Nano pending/close=pass\n",rays.size());
}
}
int main(int argc,char** argv) {
    try {
        auto system=memory::MemorySystem::create().value(); auto heap=system.create_heap({"voxel-intersection-probe",memory::DomainCategory::render}).value();
        if (argc>1 && std::string_view{argv[1]}=="compile") {
            const std::array includes{std::filesystem::path{DK_VOXEL_SHADER_DIR},std::filesystem::path{DK_VOXEL_NANO_INCLUDE_DIR}};
            const std::array defines{ShaderDefine{"DK_NANOVDB","1"}};
            auto compiled=take(compile_shader({std::filesystem::path{DK_RT_SHADER},"intersectionMain",ShaderStage::intersection,includes,defines},heap));
            check(validate_shader_artifact(compiled));
            const auto path=std::filesystem::path{DK_VOXEL_ARTIFACT_DIR}/"voxel-intersection.spv";
            std::filesystem::create_directories(path.parent_path());
            std::ofstream stream{path,std::ios::binary}; stream.write(reinterpret_cast<const char*>(compiled.spirv.data()),compiled.spirv.size()*4);
            require(bool(stream),"intersection artifact write failed");
            std::printf("PNanoVDB intersection compiled: %zu words, %zu bindings, %zu attributes\n",compiled.spirv.size(),compiled.bindings.size(),compiled.ray_interface.payloads.size());
            return 0;
        }
        Diagnostics diagnostics; run(heap,diagnostics);
        std::printf("errors=%u warnings=%u liveAllocations=%zu\n",diagnostics.errors.load(),diagnostics.warnings.load(),heap.snapshot().live_allocations);
        require(diagnostics.errors==0 && diagnostics.warnings==0 && heap.snapshot().live_allocations==0,"RT validation diagnostics or leaked CPU allocations");
        heap.begin_close(); require(system.try_close().closed(),"Memory system failed to close"); return 0;
    } catch (const std::exception& error) { std::fprintf(stderr,"required NanoVDB intersection probe: %s\n",error.what()); return 1; }
}

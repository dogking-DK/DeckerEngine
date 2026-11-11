#include <dk/render/VoxelGpu.hpp>
#include <dk/assets/VoxelBuilder.hpp>
#include <dk/graphics/ShaderCompiler.hpp>
#include <dk/graphics/Transfer.hpp>
#include <dk/graphics/CommandEncoder.hpp>
#include <dk/memory/MemorySystem.hpp>
#include "SubmissionInternal.hpp"
#include "VoxelOracle.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <random>
#include <stdexcept>

using namespace dk;
using namespace dk::graphics;
using namespace dk::render;
using namespace dk::assets;
namespace vx=dk::geometry::voxels;
namespace {
template<class T,class E> T take(std::expected<T,E> r) {
    if (!r) { if constexpr(std::is_same_v<E,Error>) throw std::runtime_error(r.error().message); else throw std::bad_alloc{}; }
    return std::move(*r);
}
void check(Result<void> r) { if (!r) throw std::runtime_error(r.error().message); }
void require(bool value,const char* text) { if (!value) throw std::runtime_error(text); }
struct Diagnostics { std::atomic<unsigned> errors=0,warnings=0,known=0; };
void diagnostic(void* user,const Diagnostic& msg) noexcept {
    auto& d=*static_cast<Diagnostics*>(user);
    if (msg.severity==VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) ++d.errors;
    if (msg.severity==VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        ++d.warnings;
        if (msg.name=="Loader Message" && msg.message=="Layer VK_LAYER_AMD_switchable_graphics uses API version 1.3 which is older than the application specified API version of 1.4. May cause issues.") ++d.known;
    }
    if (msg.severity>=VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) std::fprintf(stderr,"%.*s: %.*s\n",int(msg.name.size()),msg.name.data(),int(msg.message.size()),msg.message.data());
}
PFN_vkQueueSubmit2 native_submit=nullptr;
PFN_vkWaitSemaphores native_wait=nullptr;
PFN_vkGetSemaphoreCounterValue native_counter=nullptr;
bool fail_submit=false,force_timeout=false;
VKAPI_ATTR VkResult VKAPI_CALL submit(VkQueue q,std::uint32_t count,const VkSubmitInfo2* info,VkFence fence) {
    if (std::exchange(fail_submit,false)) return VK_ERROR_OUT_OF_HOST_MEMORY;
    return native_submit(q,count,info,fence);
}
VKAPI_ATTR VkResult VKAPI_CALL counter(VkDevice d,VkSemaphore s,std::uint64_t* value) {
    if (force_timeout) { *value=0; return VK_SUCCESS; } return native_counter(d,s,value);
}
VKAPI_ATTR VkResult VKAPI_CALL wait(VkDevice d,const VkSemaphoreWaitInfo* info,std::uint64_t timeout) {
    return force_timeout ? VK_TIMEOUT : native_wait(d,info,timeout);
}
std::uint32_t allocations(const SubmissionQueue& queue) {
    VmaTotalStatistics stats{}; vmaCalculateStatistics(queue.device().allocator(),&stats); return stats.total.statistics.allocationCount;
}
Transformd centered() { Trsd trs; trs.translation=Vec3d::Constant(.5); return take(Transformd::from_trs(trs)); }
std::vector<VoxelGpuResult> read(SubmissionQueue& q,VoxelGpuFrame frame) {
    std::vector<VoxelGpuResult> result; require(take(frame.read(q,result)),"GPU frame incomplete"); return result;
}
void compare(const VoxelGpuResult& actual,const std::optional<vx::Hit>& expected,std::size_t index) {
    if ((actual.status==VoxelGpuStatus::hit)!=expected.has_value()) throw std::runtime_error("Hit/miss mismatch at ray "+std::to_string(index));
    if (!expected) return;
    if (actual.cell!=expected->cell || actual.value!=expected->value || actual.face!=static_cast<unsigned>(expected->face))
        throw std::runtime_error("cell/value/face mismatch at ray "+std::to_string(index));
    if (std::abs(actual.distance-expected->distance)>1e-4*std::max(1.0,std::abs(expected->distance)))
        throw std::runtime_error("distance mismatch at ray "+std::to_string(index));
    for (int axis=0;axis<3;++axis) if (std::abs(actual.normal[axis]-expected->normal[axis])>1e-4)
        throw std::runtime_error("normal mismatch at ray "+std::to_string(index));
}
void offline(memory::ResourceHandle heap) {
    const std::array paths{std::filesystem::path{DK_VOXEL_NANO_INCLUDE_DIR}};
    ShaderCompileRequest request{std::filesystem::path{DK_VOXEL_SHADER_DIR}/"voxel-query.slang","computeMain",ShaderStage::compute,paths,{},true};
    auto shader=take(compile_shader(request,heap)); auto again=take(compile_shader(request,heap));
    require(shader.compiler=="2026.18","Unexpected Slang version");
    require(shader.spirv==again.spirv && shader.source_fingerprint==again.source_fingerprint,"Unstable voxel shader compilation");
    require(shader.bindings.size()==4 && shader.bindings[0].block_size==96 && shader.thread_group_size==std::array<std::uint32_t,3>{64,1,1},"Compute reflection mismatch");
    for (std::string_view name:{"PNanoVDB.h","pnano-adapter.slang","voxel-hdda.slang"})
        require(std::ranges::any_of(shader.dependencies,[&](const auto& dependency){return std::string_view(dependency).find(name)!=std::string_view::npos;}),"Missing dependency fingerprint input");
    require(!shader.requires_ray_tracing,"Voxel compute unexpectedly requires RT");
    const auto directory=std::filesystem::path{DK_VOXEL_ARTIFACT_DIR}; std::filesystem::create_directories(directory);
    std::ofstream binary(directory/"voxel-query.spv",std::ios::binary);
    binary.write(reinterpret_cast<const char*>(shader.spirv.data()),static_cast<std::streamsize>(shader.spirv.size()*4));
    require(bool(binary),"SPIR-V output failed");
    const auto reflection=shader_reflection_json(shader);
    std::ofstream json(directory/"voxel-reflection.json"); json.write(reflection.data(),static_cast<std::streamsize>(reflection.size()));
    require(bool(json),"Reflection output failed");
    std::printf("Slang=%s SPIRV=%zu fingerprint=%016llx descriptors=4 uniform=96 ray=48 result=64\n",shader.compiler.c_str(),shader.spirv.size()*4,static_cast<unsigned long long>(shader.source_fingerprint));
}
void address_checks(memory::ResourceHandle heap,SubmissionQueue& q) {
    const std::array paths{std::filesystem::path{DK_VOXEL_SHADER_DIR},std::filesystem::path{DK_VOXEL_NANO_INCLUDE_DIR}};
    auto shader=take(compile_shader({std::filesystem::path{DK_VOXEL_FIXTURE_DIR}/"voxel-address-check.slang","computeMain",ShaderStage::compute,paths},heap));
    auto factory=q.resources(); auto module=take(factory.create_shader(shader)); const std::array modules{&module};
    auto layout=take(factory.create_pipeline_layout(modules)); auto pipeline=take(factory.create_compute_pipeline({&module,&layout}));
    auto input=take(q.create_buffer({64,vk::BufferUsageFlagBits::eStorageBuffer|vk::BufferUsageFlagBits::eTransferDst}));
    auto output=take(q.create_buffer({64,vk::BufferUsageFlagBits::eStorageBuffer|vk::BufferUsageFlagBits::eTransferSrc}));
    std::array<std::uint32_t,16> words{}; words[15]=42;
    const std::array writes{BindingWrite{0,0,BufferBinding{&input}},BindingWrite{1,0,BufferBinding{&output}}};
    auto bindings=take(factory.create_bindings(layout,0,writes)); const std::array sets{&bindings};
    auto batch=take(q.begin()); check(batch.upload(input,std::as_bytes(std::span{words})));
    std::array uses{buffer_use(input,vk::PipelineStageFlagBits2::eComputeShader,vk::AccessFlagBits2::eShaderStorageRead),
        buffer_use(output,vk::PipelineStageFlagBits2::eComputeShader,vk::AccessFlagBits2::eShaderStorageWrite)};
    uses[1].full_overwrite=true; check(batch.prepare(uses)); check(batch.dispatch(pipeline,sets,{}, {1,1,1}));
    auto request=take(batch.readback(output)); auto submission=take(q.submit(std::move(batch))); require(take(q.wait(submission)),"Address probe incomplete");
    std::array<std::array<std::uint32_t,2>,8> result{}; require(take(request.try_read(std::as_writable_bytes(std::span{result}))),"Address readback incomplete");
    for (unsigned i=0;i<8;++i) require(result[i][1]==(i==5 || i==6 ? 0u : 1u),"Bad address was treated as background");
    require(result[5][0]==42 && result[6][0]==0,"Valid accessor read changed");
}
void numeric(memory::ResourceHandle heap,SubmissionQueue& q) {
    auto query=take(VoxelGpuQuery::create(heap,q)); auto cache=take(VoxelGpuCache::create(q));
    std::mt19937 random(230303); std::uniform_real_distribution<float> point(-4.7f,4.3f), direction(-1,1);
    for (int fixture=0;fixture<6;++fixture) {
        std::vector<std::uint32_t> cells(64); std::vector<VoxelBlock> blocks;
        for (int z=0;z<4;++z) for (int y=0;y<4;++y) for (int x=0;x<4;++x) {
            std::uint32_t value=0;
            if (fixture==1) value=1;
            if (fixture==2) value=x==0 || x==3 || y==0 || y==3 || z==0 || z==3;
            if (fixture==3) value=(x+y+z)%2 ? 2u : 0u;
            if (fixture==4) value=static_cast<std::uint32_t>(random()%3);
            if (fixture==5) value=x==2 && y==1 && z==3;
            cells[x+4*(y+4*z)]=value; blocks.push_back({{x-2,y-2,z-2},1,static_cast<std::int32_t>(value),random()%2!=0});
        }
        VoxelBuildOptions options; options.palette_size=2; options.sample_to_local=centered(); options.domain=vx::Domain{{-2,-2,-2},{1,1,1}};
        auto source=take(build_voxel_grid(blocks,options)); auto gpu=take(cache->prepare(q,source));
        auto dense=take(vx::DenseGrid::build({{-2,-2,-2},{4,4,4},cells,2}));
        std::vector<geometry::Ray> rays;
        for (int i=0;i<1024;++i) rays.push_back({{point(random),point(random),point(random)},{direction(random),direction(random),direction(random)},i%3 ? 0 : .25,20});
        auto results=read(q,take(query.trace(q,gpu,rays)));
        for (std::size_t i=0;i<rays.size();++i) { compare(results[i],voxel_face_oracle(dense,rays[i]),i); compare(results[i],take(source.nearest(rays[i])).hit,i); }
        std::vector<vx::Coord> coords;
        for (int z=-3;z<3;++z) for (int y=-3;y<3;++y) for (int x=-3;x<3;++x) coords.push_back({x,y,z});
        const auto sampled=read(q,take(query.sample(q,gpu,coords)));
        for (std::size_t i=0;i<coords.size();++i) require(sampled[i].status==VoxelGpuStatus::sample && sampled[i].value==dense.value(coords[i]) && sampled[i].active==(dense.value(coords[i])!=0),"Normalized sample mismatch");
    }
    std::printf("six fixtures: 6144 independent-oracle rays and 1296 normalized samples passed\n");
    {
        const std::array blocks{VoxelBlock{{-4096,0,0},4096,1,false},VoxelBlock{{0,0,0},128,2,true},
            VoxelBlock{{128,0,0},8,3,false},VoxelBlock{{137,0,0},1,16777217,true},VoxelBlock{{138,0,0},1,0,true},VoxelBlock{{139,0,0},1,INT32_MAX,false}};
        VoxelBuildOptions options; options.palette_size=INT32_MAX;
        auto source=take(build_voxel_grid(blocks,options)); auto gpu=take(cache->prepare(q,source));
        const std::array coords{vx::Coord{-4096,0,0},vx::Coord{0,0,0},vx::Coord{128,0,0},vx::Coord{137,0,0},vx::Coord{138,0,0},vx::Coord{139,0,0},vx::Coord{200,0,0}};
        const auto result=read(q,take(query.sample(q,gpu,coords)));
        const std::array dimensions{4096u,128u,8u,1u};
        for (std::size_t i=0;i<coords.size();++i) { require(result[i].value==source.value(coords[i]),"Integer palette precision lost"); if(i<4) require(result[i].dimension==dimensions[i],"Hierarchy dimension mismatch"); }
    }
    {
        const std::array blocks{VoxelBlock{{-4096,0,0},4096,1,false},VoxelBlock{{0,0,0},4096,2,true}};
        VoxelBuildOptions options; options.palette_size=2;
        auto source=take(build_voxel_grid(blocks,options)); auto gpu=take(cache->prepare(q,source));
        const std::array rays{geometry::Ray{{-1,0,0},{1,0,0}},geometry::Ray{{-4097,0,0},{1,0,0}}};
        const auto r=read(q,take(query.trace(q,gpu,rays)));
        for (std::size_t i=0;i<r.size();++i) compare(r[i],take(source.nearest(rays[i])).hit,i);
        std::printf("root tiles: domain=8192x4096x4096 nano=%zu nodes=0 visited=%u skipped=%u\n",source.bytes().size(),r[0].visited_regions,r[0].skipped_regions);
        require(r[0].visited_regions<=2 && r[0].skipped_regions==2 && source.bytes().size()<8192,"Constant tiles expanded or false internal face");
    }
    {
        const std::array blocks{VoxelBlock{{-524288,0,0},1,1,false},VoxelBlock{{524288,0,0},1,1,true}};
        auto source=take(build_voxel_grid(blocks)); auto gpu=take(cache->prepare(q,source));
        const std::array rays{geometry::Ray{{-524286,0,0},{1,0,0}}};
        const auto r=read(q,take(query.trace(q,gpu,rays)));
        compare(r[0],take(source.nearest(rays[0])).hit,0); require(r[0].visited_regions<1024 && r[0].skipped_regions>100,"Sparse empty space was not skipped");
        std::printf("sparse islands: span=1048577 nano=%zu leaves=%u visited=%u skipped=%u\n",source.bytes().size(),source.statistics().node_counts[0],r[0].visited_regions,r[0].skipped_regions);
        auto limited=take(query.trace(q,gpu,rays,{}, {1024*1024,1})); std::vector<VoxelGpuResult> sentinel(1); sentinel[0].value=99;
        require(!limited.read(q,sentinel) && sentinel[0].value==99,"GPU work limit silently missed or modified output");
    }
    {
        const std::array blocks{VoxelBlock{{0,0,0},8,1,false},VoxelBlock{{8,0,0},8,1,true},VoxelBlock{{7,3,3},1,0,true}};
        VoxelBuildOptions options; options.sample_to_local=centered(); auto source=take(build_voxel_grid(blocks,options));
        auto gpu=take(cache->prepare(q,source));
        const std::array rays{geometry::Ray{{1.5,3.5,3.5},{1,0,0}},geometry::Ray{{7.5,3.5,3.5},{1,0,0}},geometry::Ray{{1.5,2.5,2.5},{1,0,0}},
            geometry::Ray{{-1,-1,-1},{1,1,1}},geometry::Ray{{-1,0,4},{1,0,0}},geometry::Ray{{8,2,2},{-1,0,0}},
            geometry::Ray{{-1,2.5,2.5},{1,0,0},2,10},geometry::Ray{{-1,2.5,2.5},{1,0,0},0,1},geometry::Ray{{1,.5,9},{1,0,-1}}};
        const auto r=read(q,take(query.trace(q,gpu,rays)));
        for (std::size_t i=0;i<r.size();++i) compare(r[i],take(source.nearest(rays[i])).hit,i);
        require(r.back().face==static_cast<unsigned>(vx::Face::positive_z),"Internal grid plane supplied the surface normal");
    }
    {
        const std::array blocks{VoxelBlock{{-2,1,0},1,1,false}}; VoxelBuildOptions options;
        Mat4d map=Mat4d::Identity(); map(0,0)=-2; map(1,1)=3; map(2,2)=.25; map(0,1)=.5; map(2,0)=.125; map(1,3)=-17;
        options.sample_to_local=take(Transformd::from_matrix(map)); auto source=take(build_voxel_grid(blocks,options)); auto gpu=take(cache->prepare(q,source));
        Trsd trs; trs.translation={10,-4,2}; trs.scale={2,.5,-3}; auto world=take(Transformd::from_trs(trs)); auto complete=take(world.compose(source.corner_to_local()));
        const std::array rays{geometry::Ray{take(complete.transform_point({-3,1.25,.75})),take(complete.transform_direction({2,0,0}))}};
        compare(read(q,take(query.trace(q,gpu,rays,world)))[0],take(source.nearest(rays[0],world)).hit,0);
    }
    address_checks(heap,q);
}
void offscreen(memory::ResourceHandle heap,SubmissionQueue& q) {
    auto query=take(VoxelGpuQuery::create(heap,q)); auto cache=take(VoxelGpuCache::create(q));
    std::vector<VoxelBlock> blocks; std::vector<std::uint32_t> cells(64*64*64);
    for (int z=0;z<64;z+=8) for (int y=0;y<64;y+=8) for (int x=0;x<64;x+=8) blocks.push_back({{x,y,z},8,1+(x/8+y/8+z/8)%2,false});
    for (int z=0;z<64;++z) for (int y=0;y<64;++y) for (int x=0;x<64;++x) cells[x+64*(y+64*z)]=1+(x/8+y/8+z/8)%2;
    VoxelBuildOptions options; options.sample_to_local=centered(); options.palette_size=2;
    auto source=take(build_voxel_grid(blocks,options)); auto gpu=take(cache->prepare(q,source));
    auto dense=take(vx::DenseGrid::build({{0,0,0},{64,64,64},cells,2}));
    std::filesystem::create_directories(DK_VOXEL_ARTIFACT_DIR);
    for (int projection=0;projection<2;++projection) {
        std::vector<geometry::Ray> rays; rays.reserve(640*480);
        for (int y=0;y<480;++y) for (int x=0;x<640;++x) {
            // Binary fractions avoid accidental quantization of the independent reference rays.
            const float sx=(static_cast<float>(x)+.5f-320)*.125f, sy=(static_cast<float>(y)+.5f-240)*.125f;
            rays.push_back(projection ? geometry::Ray{{32+sx,32+sy,-20},{0,0,1},1,200} : geometry::Ray{{32,32,-80},{sx/64,sy/64,1},1,200});
        }
        const auto begin=std::chrono::steady_clock::now(); const auto result=read(q,take(query.trace(q,gpu,rays)));
        const auto elapsed=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count();
        std::vector<unsigned char> image(result.size()*3); std::size_t hits=0;
        for (std::size_t i=0;i<rays.size();++i) {
            compare(result[i],take(dense.nearest(rays[i])).hit,i);
            if (result[i].status==VoxelGpuStatus::hit) { ++hits; image[3*i]=result[i].value==1 ? 220 : 70; image[3*i+1]=130; image[3*i+2]=result[i].value==1 ? 60 : 230; }
        }
        require(hits>100000 && hits<rays.size(),"Offscreen fixture lacks both hits and background");
        const auto name=projection ? "voxel-orthographic.ppm" : "voxel-perspective.ppm";
        std::ofstream file(std::filesystem::path{DK_VOXEL_ARTIFACT_DIR}/name,std::ios::binary); file << "P6\n640 480\n255\n";
        file.write(reinterpret_cast<const char*>(image.data()),static_cast<std::streamsize>(image.size())); require(bool(file),"Offscreen diagnostic write failed");
        std::printf("64^3 %s 640x480 hits=%zu nano=%zu source_tree=%llu query_payload=%zu submit_wait_read_ms=%.3f\n",projection ? "orthographic" : "perspective",hits,source.bytes().size(),static_cast<unsigned long long>(source.statistics().source_tree_bytes),96+rays.size()*(48+128),elapsed);
    }
}
void lifetime(memory::MemorySystem& memory,memory::ResourceHandle heap,SubmissionQueue& q) {
    auto query=take(VoxelGpuQuery::create(heap,q)); auto cache=take(VoxelGpuCache::create(q));
    const std::array blocks{VoxelBlock{{0,0,0},8,1,false}}, replacement{VoxelBlock{{8,0,0},8,1,false}};
    auto source=take(build_voxel_grid(blocks)), second_source=take(build_voxel_grid(replacement));
    {
        // Freeze observed completion, not native submission: the old upload keeps its staging.
        const auto budget=source.bytes().size()+2*second_source.bytes().size();
        auto limited=take(VoxelGpuCache::create(q,{budget,2}));
        auto pending_grid=take(limited->prepare(q,source));
        force_timeout=true; const auto rejected=limited->prepare(q,second_source); force_timeout=false;
        require(!rejected && limited->current()==pending_grid,"Pending upload staging escaped byte budget");
        require(take(q.wait(pending_grid->upload_submission())),"Budget upload incomplete");
        auto next=take(limited->prepare(q,second_source));
        require(next->generation()==2 && take(q.wait(next->upload_submission())),"Completed staging was not reclaimed");
    }
    auto first=take(cache->prepare(q,source)); require(take(q.wait(first->upload_submission())),"Upload incomplete");
    const auto submitted=q.stats().submitted; require(take(cache->prepare(q,source))==first && q.stats().submitted==submitted,"Cache hit submitted new work");
    const auto live=allocations(q); fail_submit=true;
    require(!cache->prepare(q,second_source) && !fail_submit && cache->current()==first && allocations(q)==live,"Upload failure damaged current or leaked allocations");
    const std::array rays{geometry::Ray{{-1,0,0},{1,0,0}}};
    fail_submit=true; require(!query.trace(q,first,rays) && !fail_submit && allocations(q)==live,"Query submission failure leaked");
    auto first_frame=take(query.trace(q,first,rays)); std::vector<VoxelGpuResult> output(1); output[0].value=99;
    force_timeout=true; auto pending=first_frame.read(q,output,0); force_timeout=false;
    require(pending && !*pending && output[0].value==99,"Timeout published partial output");
    auto second=take(cache->prepare(q,second_source)); require(second->generation()==2,"Failure consumed generation");
    std::weak_ptr<const VoxelGpuGrid> retained=first; first.reset();
    require(!cache->prepare(q,source) && !retained.expired(),"In-flight old generation escaped budget");
    require(take(first_frame.read(q,output)) && output[0].value==1 && output[0].distance==.5f,"Old snapshot changed after replacement");
    first_frame={}; check(q.poll()); auto third=take(cache->prepare(q,source)); require(third->generation()==3 && retained.expired(),"Retired generation was not reclaimed");
    require(!query.trace(q,third,rays,{}, {1,10}),"Query byte budget ignored");
    auto tiny=take(VoxelGpuCache::create(q,{1,2})); require(!tiny->prepare(q,source) && !tiny->current(),"Upload byte budget ignored");
    const std::array invalid_rays{geometry::Ray{{1048577,0,0},{1,0,0}}}; require(!query.trace(q,third,invalid_rays),"Unsupported float precision accepted");
    const std::array tiny_near{geometry::Ray{{-.5,0,0},{1,0,0},1e-300,10}}; require(!query.trace(q,third,tiny_near),"Underflowing near bound accepted");
    Trsd singular; singular.scale.x()=0; require(!query.trace(q,third,rays,take(Transformd::from_trs(singular))),"Singular instance accepted");
    DeviceOptions options; options.validation=ValidationMode::required;
    auto other=take(SubmissionQueue::create(heap,take(Device::create(heap,options))));
    require(!cache->prepare(other,source) && !query.trace(other,third,rays),"Foreign queue accepted"); check(other.close());
    auto frame=take(query.trace(q,third,rays)); auto closing_grid=third;
    auto closing_cache=take(VoxelGpuCache::create(q));
    cache.reset(); third.reset(); second.reset(); query=take(VoxelGpuQuery::create(heap,q));
    // Old renderer and cache are gone while the submitted Graph owns the required resources.
    require(!memory.try_close().closed(),"Memory closed despite live GPU work");
    require(!query.trace(q,closing_grid,rays) && !closing_cache->prepare(q,source),"New work accepted during closing");
    closing_grid.reset(); closing_cache.reset();
    require(take(frame.read(q,output)) && output[0].value==1,"Closing or owner release invalidated pending readback");
    std::printf("submit failures, quotas, foreign queue, timeout, retired/in-flight owners and closing passed\n");
}
}
int main(int argc,char** argv) {
    Diagnostics diagnostics; auto memory=take(memory::MemorySystem::create()); auto heap=take(memory.create_heap({"voxel-gpu",memory::DomainCategory::render}));
    try {
        const std::string_view mode=argc>1 ? argv[1] : "numeric";
        if (mode=="compile") offline(heap);
        else {
            DeviceOptions options; options.validation=ValidationMode::required; options.diagnostic_sink=diagnostic; options.diagnostic_user_data=&diagnostics;
            auto device=take(Device::create(heap,options)); require(!device.ray_tracing_enabled(),"M23.3 enabled RT");
            std::printf("GPU=%s driver=%s RT=disabled validation=required\n",device.adapter().properties.deviceName.data(),device.adapter().driver.driverName.data());
            native_submit=reinterpret_cast<PFN_vkQueueSubmit2>(device.device_proc("vkQueueSubmit2"));
            native_wait=reinterpret_cast<PFN_vkWaitSemaphores>(device.device_proc("vkWaitSemaphores")); native_counter=reinterpret_cast<PFN_vkGetSemaphoreCounterValue>(device.device_proc("vkGetSemaphoreCounterValue"));
            auto q=take(graphics::detail::SubmissionAccess::create(heap,std::move(device),3,{submit,counter,wait}));
            if (mode=="numeric") numeric(heap,q); else if (mode=="offscreen") offscreen(heap,q); else if (mode=="lifetime") lifetime(memory,heap,q); else throw std::runtime_error("Unknown mode");
            check(q.poll()); require(!q.stats().pending_slots && !q.stats().recording_slots && allocations(q)==0,"Voxel GPU resources leaked"); check(q.close());
        }
        std::printf("validation errors=%u warnings=%u known=%u live=%zu\n",diagnostics.errors.load(),diagnostics.warnings.load(),diagnostics.known.load(),heap.snapshot().live_allocations);
        return !diagnostics.errors && diagnostics.warnings==diagnostics.known && !heap.snapshot().live_allocations && memory.try_close().closed() ? 0 : 1;
    } catch(const std::exception& error) { std::fprintf(stderr,"voxel probe failed: %s\n",error.what()); return 1; }
}

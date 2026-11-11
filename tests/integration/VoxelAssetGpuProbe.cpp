#include "../VoxelAssetSupport.hpp"
#include <dk/assets/AssetCache.hpp>
#include <dk/render/VoxelAssets.hpp>
#include <dk/render/VoxelGpu.hpp>
#include <dk/render/ScenePipeline.hpp>
#include <dk/scene/SceneIO.hpp>
#include <dk/math/Projection.hpp>
#include <dk/memory/MemorySystem.hpp>
#include <dk/memory/Context.hpp>
#include "SubmissionInternal.hpp"
#include <atomic>
#include <cstdio>
#include <numbers>
using namespace dk;
using namespace dk::render;
using namespace dk::graphics;
namespace {
template<class T,class E> T take(std::expected<T,E> r) { if(!r) { if constexpr(std::is_same_v<E,Error>) throw std::runtime_error(r.error().message); else throw std::bad_alloc{}; } return std::move(*r); }
void check(Result<void> r) { if(!r) throw std::runtime_error(r.error().message); }
void require(bool b,const char* text) { if(!b) throw std::runtime_error(text); }
struct Diagnostics { std::atomic<unsigned> errors=0,warnings=0; };
void diagnostic(void* p,const Diagnostic& d) noexcept {
    auto& s=*static_cast<Diagnostics*>(p);
    if(d.severity==VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) ++s.errors;
    if(d.severity==VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) ++s.warnings;
    if(d.severity>=VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) std::fprintf(stderr,"%.*s: %.*s\n",int(d.name.size()),d.name.data(),int(d.message.size()),d.message.data());
}
PFN_vkQueueSubmit2 native_submit=nullptr;
PFN_vkWaitSemaphores native_wait=nullptr;
PFN_vkGetSemaphoreCounterValue native_counter=nullptr;
bool fail_submit=false,force_timeout=false;
VKAPI_ATTR VkResult VKAPI_CALL submit(VkQueue q,std::uint32_t n,const VkSubmitInfo2* info,VkFence fence) {
    if(std::exchange(fail_submit,false)) return VK_ERROR_OUT_OF_HOST_MEMORY; return native_submit(q,n,info,fence);
}
VKAPI_ATTR VkResult VKAPI_CALL wait(VkDevice d,const VkSemaphoreWaitInfo* info,std::uint64_t t) { return force_timeout ? VK_TIMEOUT : native_wait(d,info,t); }
VKAPI_ATTR VkResult VKAPI_CALL counter(VkDevice d,VkSemaphore s,std::uint64_t* v) { if(force_timeout) { *v=0; return VK_SUCCESS; } return native_counter(d,s,v); }
std::uint32_t allocations(SubmissionQueue& q) { VmaTotalStatistics s{}; vmaCalculateStatistics(q.device().allocator(),&s); return s.total.statistics.allocationCount; }
std::vector<std::byte> pixels(const RenderFrame& f) { std::vector<std::byte> v(std::size_t(f.info().width)*f.info().height*4); check(f.read_rgba8(v)); return v; }
void run(memory::MemorySystem& memory,memory::ResourceHandle heap,SubmissionQueue& q) {
    const auto root=std::filesystem::path{DK_VOXEL_ARTIFACT_DIR}/("assets-"+take(AssetId::generate()).to_string());
    voxel_fixture::write(root); const auto paths=take(ProjectPaths::create(root));
    auto cpu=take(compile_cached_asset(paths,{"assets/solid.dkvoxel"})); const auto id=root_asset_id(cpu.artifact.data);
    auto project=take(Project::create(root,{"GPU voxel","scene.json",{{id,AssetKind::voxel,"assets/solid.dkvoxel"}}}));
    auto scene=take(SceneDocument::create()); const auto entity=take(EntityId::generate()); check(scene->create_entity(entity));
    check(scene->set_asset_references(entity,{{id,AssetKind::voxel}})); Trsd t; t.translation={.5,-.5,-3}; check(scene->set_local_transform(entity,t));
    check(save_project(project,"project.json")); check(save_scene(*scene,project)); scene=take(load_scene(project));
    auto assets=take(VoxelAssets::create(heap,q)); auto meshes=take(GpuAssets::create(heap));
    auto old=take(assets.upload(q,cpu.artifact.data)); require(take(old.wait(q)),"Initial upload incomplete");
    auto pipeline=take(ScenePipeline::create(heap,q,DK_RENDER_SHADER_DIR));
    ViewDescription d{64,64,1,{}}; d.projection=take(perspective_projection({std::numbers::pi/2,.1,10},1));
    auto view=take(RenderView::create(take(RenderScene::extract(heap,take(scene->snapshot()))),d));
    RenderSettings settings{{.03125f,.0625f,.125f},1,true,true,true,true}; VoxelRenderSettings shading;
    require(!pipeline.render(q,view,meshes,settings),"Mesh-only path silently omitted Scene voxel");
    require(!meshes.upload(q,cpu.artifact.data),"Mesh uploader accepted voxel payload");
    auto first=take(pipeline.render_voxel_assets(q,view,meshes,assets,shading,settings)); require(take(first.wait(q)),"Initial frame incomplete"); const auto old_pixels=pixels(first);
    // Center hits material 1 on the negative-x tile; validate actual shading, not just unequal images.
    std::vector<std::array<float,4>> linear(64*64); check(first.read_linear_rgba32f(linear));
    const auto center=linear[32*64+32]; require(std::abs(center[0]-.125f)<1e-5f && std::abs(center[1]-.25f)<1e-5f && std::abs(center[2]-1)<1e-5f,"Scene reference was not rendered from palette");
    const auto before=q.stats().submitted; const auto base=allocations(q); const auto generation=old.generation();
    auto unchanged=[&] { require(take(assets.find(id)).generation()==generation && q.stats().submitted==before && allocations(q)==base && pixels(first)==old_pixels,"Failed replacement changed current, submitted or leaked"); };
    auto bad=cpu.artifact.data; bad.voxel->palette.clear(); require(!assets.upload(q,bad),"Missing palette accepted"); unchanged();
    auto j=voxel_fixture::description(); j["palette"][0]["base_color"]={1.,.125,.25}; voxel_fixture::write_text(root/"assets/solid.dkvoxel",j);
    auto next=take(compile_cached_asset(paths,{"assets/solid.dkvoxel"})); require(root_asset_id(next.artifact.data)==id && next.key!=cpu.key,"Replacement identity/cache key incorrect");
    fail_submit=true; require(!assets.upload(q,next.artifact.data) && !fail_submit,"Upload failure not surfaced"); unchanged();
    auto replacement=take(assets.upload(q,next.artifact.data)); require(replacement.generation()>generation && take(replacement.wait(q)),"Replacement not published");
    auto second=take(pipeline.render_voxel_assets(q,view,meshes,assets,shading,settings)); require(take(second.wait(q)),"Replacement frame incomplete");
    require(pixels(second)!=old_pixels && pixels(first)==old_pixels && old.palette()[0].base_color.x()==.125f,"Replacement damaged old GPU snapshot");
    check(second.read_linear_rgba32f(linear)); require(std::abs(linear[32*64+32][0]-1)<1e-5f,"Replacement palette did not reach shader");
    auto third_source=take(load_cpu_artifact(root/take(path_from_utf8(next.directory)))).data; require(!assets.upload(q,third_source),"Retained generation budget ignored");
    old={}; first={}; cpu={}; check(q.poll());
    auto third=take(assets.upload(q,third_source)); require(third.generation()>replacement.generation(),"Retired generation not reclaimed"); require(take(third.wait(q)),"Third upload incomplete");
    replacement={}; second={}; check(q.poll());
    auto pending=take(pipeline.render_voxel_assets(q,view,meshes,assets,shading,settings));
    force_timeout=true; auto timed=pending.wait(q,0); force_timeout=false; require(timed && !*timed,"Pending frame timeout was misreported");
    std::weak_ptr<const VoxelGpuGrid> retained=third.grid(); third={}; require(assets.unload(id) && !assets.find(id) && !assets.size(),"Unload retained lookup");
    require(!retained.expired(),"Unload destroyed in-flight grid"); const auto count=q.stats().submitted;
    require(!pipeline.render_voxel_assets(q,view,meshes,assets,shading,settings) && q.stats().submitted==count,"Missing Scene asset was silently omitted/submitted");
    require(!memory.try_close().closed() && !assets.upload(q,third_source),"Closing accepted new asset upload");
    assets={}; pipeline={}; require(take(pending.wait(q)),"Unload/closing invalidated pending frame");
    check(pending.read_linear_rgba32f(linear)); require(std::abs(linear[32*64+32][0]-1)<1e-5f,"Pending palette corrupted on unload");
    pending={}; check(q.poll()); require(retained.expired(),"Retired asset GPU allocation still owned");
    std::printf("Scene file -> cache -> AssetId -> GPU; replacement/failure/generation budget/unload/in-flight/closing passed\n");
}
}
int main() {
    try {
        Diagnostics diagnostics; auto memory=take(memory::MemorySystem::create()); auto heap=take(memory.create_heap({"voxel-assets",memory::DomainCategory::render}));
        {
            memory::ThreadContext context{memory}; memory::ExecutionScope scope{context,heap};
            DeviceOptions o; o.validation=ValidationMode::required; o.diagnostic_sink=diagnostic; o.diagnostic_user_data=&diagnostics;
            auto device=take(Device::create(heap,o)); require(!device.ray_tracing_enabled(),"Asset test unexpectedly enabled RT");
            std::printf("GPU=%s driver=%s validation=required RT=disabled\n",device.adapter().properties.deviceName.data(),device.adapter().driver.driverInfo.data());
            native_submit=reinterpret_cast<PFN_vkQueueSubmit2>(device.device_proc("vkQueueSubmit2")); native_wait=reinterpret_cast<PFN_vkWaitSemaphores>(device.device_proc("vkWaitSemaphores")); native_counter=reinterpret_cast<PFN_vkGetSemaphoreCounterValue>(device.device_proc("vkGetSemaphoreCounterValue"));
            auto q=take(graphics::detail::SubmissionAccess::create(heap,std::move(device),3,{submit,counter,wait})); run(memory,heap,q);
            check(q.poll()); require(!q.stats().pending_slots && !q.stats().recording_slots && allocations(q)==0,"Asset GPU resources leaked"); check(q.close());
        }
        std::printf("validation errors=%u warnings=%u live=%zu\n",diagnostics.errors.load(),diagnostics.warnings.load(),heap.snapshot().live_allocations);
        require(!diagnostics.errors && !diagnostics.warnings && !heap.snapshot().live_allocations && memory.try_close().closed(),"Validation/lifetime gate failed"); return 0;
    } catch(const std::exception& e) { std::fprintf(stderr,"voxel assets GPU failed: %s\n",e.what()); return 1; }
}

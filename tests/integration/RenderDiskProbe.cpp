#include <dk/render/DiskScene.hpp>
#include <dk/render/ScenePipeline.hpp>
#include <dk/scene/SceneIO.hpp>
#include <dk/assets/AssetCompiler.hpp>
#include <dk/assets/Metadata.hpp>
#include <dk/io/File.hpp>
#include <dk/memory/MemorySystem.hpp>
#include <dk/memory/Context.hpp>
#include "SubmissionInternal.hpp"
#include <atomic>
#include <cstdio>
#include <stdexcept>
#include <vector>
using namespace dk;
using namespace dk::graphics;
using namespace dk::render;
namespace {
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
template<class T, class E> T take(std::expected<T,E>&& result) {
    if (!result) {
        if constexpr (std::is_same_v<E,Error>) {
            auto message = result.error().message;
            for (const auto& context : result.error().context) message += " / " + context;
            throw std::runtime_error(message);
        } else throw std::runtime_error("Memory operation failed");
    }
    return std::move(*result);
}
void check(Result<void> result) { if (!result) throw std::runtime_error(result.error().message); }
struct Diagnostics { std::atomic<unsigned> errors = 0, warnings = 0; };
void diagnostic(void* pointer, const Diagnostic& message) noexcept {
    auto& counts = *static_cast<Diagnostics*>(pointer);
    if (message.severity == VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) ++counts.errors;
    if (message.severity == VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) ++counts.warnings;
    if (message.severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
        std::fprintf(stderr,"%.*s: %.*s\n",static_cast<int>(message.name.size()),message.name.data(),
            static_cast<int>(message.message.size()),message.message.data());
}

PFN_vkQueueSubmit2 native_submit;
unsigned fail_countdown=0;
VKAPI_ATTR VkResult VKAPI_CALL submit_override(VkQueue queue,std::uint32_t count,const VkSubmitInfo2* info,VkFence fence) {
    if (fail_countdown && --fail_countdown==0) return VK_ERROR_OUT_OF_HOST_MEMORY;
    return native_submit(queue,count,info,fence);
}
std::uint32_t allocations(const SubmissionQueue& queue) {
    VmaTotalStatistics statistics{}; vmaCalculateStatistics(queue.device().allocator(),&statistics);
    return statistics.total.statistics.allocationCount;
}
struct Fixture {
    const std::filesystem::path base=std::filesystem::current_path()/"test-artifacts";
    const std::filesystem::path root=base/("disk-"+take(EntityId::generate()).to_string());
    Fixture() { std::filesystem::create_directories(root); std::filesystem::copy(DK_DISK_FIXTURE_DIR,root,std::filesystem::copy_options::recursive); }
    ~Fixture() { if (root.parent_path()==base && root.filename().string().starts_with("disk-")) { std::error_code ec; std::filesystem::remove_all(root,ec); } }
};
std::vector<std::byte> pixels(const RenderFrame& frame) {
    std::vector<std::byte> result(static_cast<std::size_t>(frame.info().width)*frame.info().height*4);
    check(frame.read_rgba8(result)); return result;
}
void pixel(const RenderFrame& frame,unsigned x,unsigned y,unsigned red,unsigned blue) {
    const auto data=pixels(frame); const auto offset=(y*frame.info().width+x)*4;
    auto close=[](unsigned a,unsigned b) { return a>b ? a-b<=1 : b-a<=1; };
    require(close(std::to_integer<unsigned>(data[offset]),red) && data[offset+1]==std::byte{0} &&
        close(std::to_integer<unsigned>(data[offset+2]),blue) && data[offset+3]==std::byte{255},"disk alpha mask pixel mismatch");
}
void run(memory::ResourceHandle heap,SubmissionQueue& queue) {
    Fixture fixture; auto project=take(Project::open(fixture.root,"project.json"));
    auto source=take(DiskScene::load(heap,project)); require(source.assets().size()==2,"disk inputs not deduplicated");
    auto cache=take(source.upload(queue)); require(queue.stats().pending_slots==0 && cache.size()==2,"upload returned before completion");
    auto pipeline=take(ScenePipeline::create(heap,queue,DK_RENDER_SHADER_DIR));
    auto view=take(RenderView::create(source.scene(),{64,64,1}));
    auto frame=take(pipeline.render(queue,view,cache)); require(take(frame.wait(queue)),"disk frame wait failed");
    require(frame.info().draw_count==3 && frame.info().revision==1,"disk frame identity wrong");
    pixel(frame,1,1,0,0); pixel(frame,20,32,0,188); pixel(frame,40,32,188,0); pixel(frame,60,32,188,0);
    const auto original=pixels(frame); const auto base=allocations(queue); const auto submitted=queue.stats().submitted;
    fail_countdown=2; require(!source.upload(queue),"partial upload failure ignored");
    require(queue.stats().submitted==submitted+1 && queue.stats().pending_slots==0 && allocations(queue)==base,
        "partial upload published cache or leaked");
    require(pixels(frame)==original && cache.size()==2,"partial upload altered old frame/cache");
    require(!DiskScene{}.upload(queue),"empty disk upload accepted");
    // Compile the same persistent identities into M4 artifacts, then load/render without source import.
    auto description=project.description();
    for (std::size_t i=0;i<source.assets().size();++i) {
        const auto& data=*source.assets()[i]; AssetMetadata meta; meta.root_id=data.mesh.id;
        for (const auto& output : data.outputs) meta.outputs.push_back({output.key,output.id,output.kind});
        const auto text=take(serialize_asset_meta(meta));
        auto meta_path=take(project.resolve_asset({data.mesh.id,AssetKind::mesh})); meta_path+=".meta";
        check(write_file_bytes(meta_path,std::as_bytes(std::span{text.data(),text.size()})));
        const auto directory="compiled-"+std::to_string(i);
        static_cast<void>(take(compile_asset(project.paths(),{description.assets[i].path,directory})));
        description.assets[i].path=directory+"/manifest.json";
    }
    auto compiled_project=take(Project::create(fixture.root,description));
    auto compiled=take(DiskScene::load(heap,compiled_project)); auto compiled_cache=take(compiled.upload(queue));
    auto compiled_view=take(RenderView::create(compiled.scene(),{64,64,2}));
    auto repeated=take(pipeline.render(queue,compiled_view,compiled_cache)); require(take(repeated.wait(queue)),"artifact wait failed");
    require(pixels(repeated)==original,"CPU artifact rendering differs from direct import"); repeated={}; compiled_cache={}; compiled={};
    // Save M2 edit and load a fresh disk snapshot. Old snapshot/frame must remain unchanged.
    auto document=take(load_scene(project)); const auto entity=take(EntityId::parse("20000000-0000-4000-8000-000000000001"));
    auto trs=take(document->entity(entity)).local; trs.translation.x()=0.5;
    check(document->set_local_transform(entity,trs)); check(save_scene(*document,project));
    auto changed=take(DiskScene::load(heap,project)); auto changed_cache=take(changed.upload(queue));
    auto changed_view=take(RenderView::create(changed.scene(),{64,64,3}));
    auto moved=take(pipeline.render(queue,changed_view,changed_cache)); require(take(moved.wait(queue)),"changed scene wait failed");
    require(moved.info().revision>frame.info().revision && pixels(moved)!=original,"saved edit did not change frame/revision");
    pixel(moved,40,32,0,188); require(pixels(frame)==original,"old frame changed after disk reload");
    const std::array<std::byte,1> broken{std::byte{0}};
    check(write_file_bytes(fixture.root/"assets/mask.png",broken));
    require(!DiskScene::load(heap,project) && pixels(frame)==original,"bad disk texture changed old frame");
    // A source/cache can disappear while the submitted frame remains usable.
    auto pending=take(pipeline.render(queue,changed_view,changed_cache));
    source={}; changed={}; cache={}; changed_cache={}; pipeline={}; frame={}; moved={};
    require(take(pending.wait(queue)),"pending disk frame failed after unload"); pixel(pending,40,32,0,188);
    require(allocations(queue)==2,"nonoutput allocations survived disk unload"); pending={};
    require(allocations(queue)==0,"disk frame allocations leaked");
    std::printf("disk input packages=2 draws=3 artifact parity=exact alpha-mask=passed partial-upload=passed\n");
}
}
int main() {
    Diagnostics diagnostics;
    try {
        auto system=take(memory::MemorySystem::create()); auto heap=take(system.create_heap({"render-disk",memory::DomainCategory::render}));
        {
            memory::ThreadContext context{system,heap}; memory::ExecutionScope scope{context,heap};
            DeviceOptions options; options.validation=ValidationMode::required; options.diagnostic_sink=diagnostic; options.diagnostic_user_data=&diagnostics;
            auto device=Device::create(heap,options);
            if (!device) { std::fprintf(stderr,"%s\n",device.error().message.c_str()); return device.error().code==ErrorCode::not_found || device.error().code==ErrorCode::not_supported ? 77 : 1; }
            native_submit=reinterpret_cast<PFN_vkQueueSubmit2>(device->device_proc("vkQueueSubmit2"));
            auto queue=take(graphics::detail::SubmissionAccess::create(heap,std::move(*device),3,{submit_override}));
            std::printf("GPU=%s driver=%s\n",queue.device().adapter().properties.deviceName.data(),queue.device().adapter().driver.driverInfo.data());
            run(heap,queue); check(queue.close());
            require(queue.stats().pending_slots==0 && allocations(queue)==0,"queue not drained");
        }
        const auto live=heap.snapshot().live_allocations;
        std::printf("errors=%u warnings=%u liveAllocations=%zu\n",diagnostics.errors.load(),diagnostics.warnings.load(),live);
        return diagnostics.errors==0 && diagnostics.warnings==0 && live==0 && system.try_close().closed() ? 0 : 1;
    } catch (const std::exception& e) { std::fprintf(stderr,"disk render probe failed: %s\n",e.what()); return 1; }
}

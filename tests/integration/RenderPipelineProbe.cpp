#include "../RenderAssetSupport.hpp"
#include <dk/render/ScenePipeline.hpp>
#include <dk/scene/SceneDocument.hpp>
#include <dk/graphics/GraphExecution.hpp>
#include <dk/graphics/Transfer.hpp>
#include "SubmissionInternal.hpp"
#include "ObjectInternal.hpp"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
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
PFN_vkWaitSemaphores native_wait;
VkResult submit_error = VK_SUCCESS;
bool timeout_once = false;
memory::ResourceHandle close_at_submit;
VKAPI_ATTR VkResult VKAPI_CALL submit_override(VkQueue queue, std::uint32_t count, const VkSubmitInfo2* info, VkFence fence) {
    if (submit_error != VK_SUCCESS) return std::exchange(submit_error,VK_SUCCESS);
    const auto result = native_submit(queue,count,info,fence);
    if (result == VK_SUCCESS && close_at_submit) close_at_submit.begin_close();
    return result;
}
VKAPI_ATTR VkResult VKAPI_CALL wait_override(VkDevice device, const VkSemaphoreWaitInfo* info, std::uint64_t timeout) {
    if (std::exchange(timeout_once,false)) return VK_TIMEOUT;
    return native_wait(device,info,timeout);
}
std::uint32_t allocations(const SubmissionQueue& queue) {
    VmaTotalStatistics stats{}; vmaCalculateStatistics(queue.device().allocator(),&stats);
    return stats.total.statistics.allocationCount;
}

CpuAsset quad(bool textured) {
    CpuAsset result; result.mesh.id=take(AssetId::generate());
    MeshPrimitive mesh; mesh.positions={{-1,-1,0},{1,-1,0},{1,1,0},{-1,1,0}};
    mesh.texcoords={{0,0},{1,0},{1,1},{0,1}}; mesh.indices={0,1,2,0,2,3};
    MaterialData material; material.id=take(AssetId::generate());
    if (textured) {
        TextureData texture; texture.id=take(AssetId::generate()); texture.width=texture.height=2;
        for (int i=0;i<4;++i) for (auto channel : {128,64,32,255}) texture.rgba8.push_back(static_cast<std::byte>(channel));
        material.base_color_texture=texture.id; result.textures.push_back(std::move(texture));
    } else { material.base_color={1,0,0,1}; material.emissive={3,0,0}; }
    mesh.material=material.id; result.materials.push_back(material); result.mesh.primitives.push_back(std::move(mesh));
    return result;
}
struct Program {
    CpuAsset front=quad(false), back=quad(true);
    std::unique_ptr<SceneDocument> scene=take(SceneDocument::create());
    EntityId root=take(EntityId::parse("00000000-0000-4000-8000-000000000010"));
    EntityId near=take(EntityId::parse("00000000-0000-4000-8000-000000000001"));
    EntityId far=take(EntityId::parse("00000000-0000-4000-8000-000000000002"));
    EntityId instance=take(EntityId::parse("00000000-0000-4000-8000-000000000003"));
    Program() {
        check(scene->create_entity(root)); Trsd parent; parent.translation.x()=0.1; check(scene->set_local_transform(root,parent));
        auto add=[&](EntityId id,AssetId mesh,Vec3d scale,Vec3d translation) {
            check(scene->create_entity(id)); Trsd trs; trs.scale=scale; trs.translation=translation;
            check(scene->set_local_transform(id,trs)); check(scene->set_parent(id,root));
            check(scene->set_asset_references(id,{{mesh,AssetKind::mesh}}));
        };
        add(near,front.mesh.id,{0.5,0.5,1},{0,0,0.25});
        add(far,back.mesh.id,{0.8,0.8,1},{0,0,0.75}); // Drawn AFTER the front surface.
        add(instance,front.mesh.id,{0.1,0.2,1},{0.85,0,0.1});
    }
    RenderView view(memory::ResourceHandle heap,std::uint32_t width=128,std::uint32_t height=96,std::uint64_t frame=1,double shift=0) {
        ViewDescription desc{width,height,frame}; Trsd camera; camera.translation.x()=0.1+shift;
        desc.camera_world=take(Transformd::from_trs(camera));
        return take(RenderView::create(take(RenderScene::extract(heap,take(scene->snapshot()))),desc));
    }
    void upload(GpuAssets& cache,SubmissionQueue& queue) {
        static_cast<void>(take(cache.upload(queue,front))); static_cast<void>(take(cache.upload(queue,back)));
    }
};
std::vector<std::byte> pixels(const RenderFrame& frame) {
    std::vector<std::byte> output(static_cast<std::size_t>(frame.info().width)*frame.info().height*4);
    check(frame.read_rgba8(output)); return output;
}
double linear(int channel) {
    const double value=channel/255.0;
    return value<=0.04045 ? value/12.92 : std::pow((value+0.055)/1.055,2.4);
}
int output_byte(double value,double exposure) {
    const double mapped=(value*exposure)/(1+value*exposure);
    const double encoded=mapped<=0.0031308 ? mapped*12.92 : 1.055*std::pow(mapped,1/2.4)-0.055;
    return static_cast<int>(std::lround(encoded*255));
}
void expect_image(const RenderFrame& frame,const RenderSettings& settings,double shift=0,bool empty=false) {
    const auto info=frame.info(); const auto data=pixels(frame);
    for (std::uint32_t y=0;y<info.height;++y) for (std::uint32_t x=0;x<info.width;++x) {
        const double px=2*(x+0.5)/info.width-1+shift, py=2*(y+0.5)/info.height-1;
        std::array<double,3> hdr{settings.clear_rgb.x(),settings.clear_rgb.y(),settings.clear_rgb.z()};
        if (!empty) {
            if (std::abs(px)<0.8 && std::abs(py)<0.8) hdr={linear(128),linear(64),linear(32)};
            if ((std::abs(px)<0.5 && std::abs(py)<0.5) || (px>0.75 && px<0.95 && std::abs(py)<0.2)) hdr={4,0,0};
        }
        const auto offset=(static_cast<std::size_t>(y)*info.width+x)*4;
        for (std::size_t c=0;c<3;++c) {
            const auto actual=std::to_integer<int>(data[offset+c]); const auto expected=output_byte(hdr[c],settings.exposure);
            if (std::abs(actual-expected)>1) throw std::runtime_error("pixel mismatch at "+std::to_string(x)+","+std::to_string(y)+
                " channel "+std::to_string(c)+" got "+std::to_string(actual)+" expected "+std::to_string(expected));
        }
        require(data[offset+3]==std::byte{255},"output alpha differs");
    }
}
void save(const RenderFrame& frame) {
    const auto bytes=pixels(frame); const auto info=frame.info();
    std::ofstream image{"render-pipeline-validation.ppm",std::ios::binary|std::ios::trunc};
    image<<"P6\n"<<info.width<<' '<<info.height<<"\n255\n";
    for (std::size_t i=0;i<bytes.size();i+=4) image.write(reinterpret_cast<const char*>(bytes.data()+i),3);
    std::ofstream plan{"render-pipeline-validation.txt",std::ios::trunc}; plan<<frame.plan_text();
    require(bool(image) && bool(plan),"artifact write failed");
}
void render_scenes(memory::ResourceHandle heap,SubmissionQueue& queue,const DeviceOptions& options) {
    Program program; auto cache=take(GpuAssets::create(heap)); program.upload(cache,queue);
    auto pipeline=take(ScenePipeline::create(heap,queue,DK_RENDER_SHADER_DIR));
    auto view=program.view(heap); const auto revision=program.scene->revision();
    RenderSettings settings{{0.04f,0.08f,0.16f},1,true};
    auto first=take(pipeline.render(queue,view,cache,settings));
    require(first.info().draw_count==3 && first.info().scene==program.scene->id() && first.info().revision==revision && first.info().frame==1,"frame metadata differs");
    std::vector<std::byte> untouched(128*96*4,std::byte{0x5a});
    require(!first.read_rgba8(untouched) && std::ranges::all_of(untouched,[](auto b){return b==std::byte{0x5a};}),"pending read modified output");
    timeout_once=true; require(!take(first.wait(queue,0)),"timeout claimed completion");
    require(take(first.wait(queue)),"frame wait failed"); expect_image(first,settings); save(first);
    for (const char* name : {"clear targets","depth prepass","opaque","tone mapping","readback"})
        require(first.plan_text().find(name)!=std::string_view::npos,"missing Graph pass diagnostic");
    require(take(take(first.color())->state()).layout==vk::ImageLayout::eShaderReadOnlyOptimal,"output not exported for sampling");
    const auto reference=pixels(first);
    check(program.scene->set_name(program.near,"edited after extraction"));
    auto repeat=take(pipeline.render(queue,view,cache,settings)); require(take(repeat.wait(queue)),"repeat wait failed");
    require(pixels(repeat)==reference && repeat.info().revision==revision,"old view changed after scene edit"); repeat={};
    settings.exposure=2; settings.capture_plan=false;
    auto moved=take(pipeline.render(queue,program.view(heap,129,97,2,0.6),cache,settings)); require(take(moved.wait(queue)),"moved view wait failed");
    expect_image(moved,settings,0.6); require(moved.info().revision>revision && moved.info().frame==2 && moved.plan_text().empty(),"new view identity/diagnostic flag wrong"); moved={};
    auto empty_source=take(SceneDocument::create()); auto empty=take(RenderScene::extract(heap,take(empty_source->snapshot())));
    settings.exposure=0.5f; auto empty_view=take(RenderView::create(empty,{17,11,3}));
    auto background=take(pipeline.render(queue,empty_view,cache,settings)); require(take(background.wait(queue)),"empty wait failed");
    expect_image(background,settings,0,true); require(background.info().draw_count==0,"empty scene drew geometry"); background={};
    const auto submitted=queue.stats().submitted; const auto base=allocations(queue);
    auto unchanged=[&] { require(queue.stats().submitted==submitted && allocations(queue)==base && pixels(first)==reference,"failed frame changed previous image/submitted/leaked"); };
    check(program.scene->set_asset_references(program.near,{{take(AssetId::generate()),AssetKind::mesh}}));
    require(!pipeline.render(queue,program.view(heap),cache,settings),"missing GPU mesh accepted"); unchanged();
    check(program.scene->set_asset_references(program.near,{{program.front.mesh.id,AssetKind::mesh}}));
    graphics::detail::ObjectAccess::fail_creation(queue.resources(),VK_ERROR_OUT_OF_HOST_MEMORY);
    require(!pipeline.render(queue,view,cache,settings),"object failure ignored"); unchanged();
    submit_error=VK_ERROR_OUT_OF_HOST_MEMORY;
    require(!pipeline.render(queue,view,cache,settings),"submit failure ignored"); unchanged();
    Trsd huge; huge.translation.x()=1e100; check(program.scene->set_local_transform(program.near,huge));
    require(!pipeline.render(queue,program.view(heap),cache,settings),"float overflow accepted"); unchanged();
    program.front.materials[0].alpha_mode=AlphaMode::mask;
    auto unsupported=take(cache.upload(queue,program.front)); require(take(unsupported.wait(queue)),"unsupported upload wait failed"); unsupported={};
    const auto before_alpha=queue.stats().submitted;
    require(!pipeline.render(queue,view,cache,settings) && queue.stats().submitted==before_alpha,"alpha mask silently rendered as opaque");
    program.front.materials[0].alpha_mode=AlphaMode::opaque; auto restored=take(cache.upload(queue,program.front)); require(take(restored.wait(queue)),"restore wait failed"); restored={};
    auto other_device=take(Device::create(heap,options)); auto other_queue=take(SubmissionQueue::create(heap,std::move(other_device)));
    require(!first.wait(other_queue) && !pipeline.render(other_queue,view,cache,settings),"foreign queue accepted frame or pipeline");
    require(other_queue.stats().submitted==0 && allocations(other_queue)==0,"foreign queue failure leaked"); check(other_queue.close());
    first={};
    auto last=take(pipeline.render(queue,view,cache,settings)); const auto ticket=last.submission();
    cache.clear(); pipeline={}; // Pending command owners preserve mesh, textures, descriptors and pipelines.
    require(queue.stats().pending_slots==1,"render unexpectedly waited");
    require(take(last.wait(queue)),"unloaded frame failed"); expect_image(last,settings);
    require(allocations(queue)==2,"non-output GPU assets survived completion");
    last={}; require(allocations(queue)==0,"frame outputs leaked"); require(take(queue.wait(ticket)),"completed ticket not repeatable");
}
void hdr_transfers(memory::ResourceHandle heap,SubmissionQueue& queue) {
    auto image=take(queue.create_image({3,2,vk::Format::eR32G32B32A32Sfloat}));
    std::array<float,24> values{}; for (std::size_t i=0;i<values.size();++i) values[i]=static_cast<float>(i)-5;
    auto batch=take(queue.begin()); check(batch.upload(image,std::as_bytes(std::span{values}),{0,0,0,0,3,2}));
    auto read=take(batch.readback(image,{0,0,0,0,3,2})); const auto ticket=take(queue.submit(std::move(batch)));
    require(take(queue.wait(ticket)),"HDR transfer wait failed");
    std::array<float,24> actual{}; require(take(read.try_read(std::as_writable_bytes(std::span{actual}))) && actual==values,"HDR transfer truncated texels");
    require(read.description().row_pitch==48 && read.description().bytes==96,"HDR readback pitch incorrect");
    for (bool short_range : {true,false}) {
        using namespace graphics::graph;
        auto graph=take(Graph::create(heap)); auto output=take(graph.declare_buffer("output",{96,vk::BufferUsageFlagBits::eTransferDst,BufferMemory::readback}));
        auto input=take(graph.declare_image("HDR",image.description(),Lifetime::external,true));
        std::array uses{Use{input,{{vk::PipelineStageFlagBits2::eCopy,vk::AccessFlagBits2::eTransferRead,vk::ImageLayout::eTransferSrcOptimal}}},
            Use{output,{{vk::PipelineStageFlagBits2::eCopy,vk::AccessFlagBits2::eTransferWrite},0,short_range ? 24u : 96u,{},true}}};
        static_cast<void>(take(graph.add_pass({"HDR copy",uses,true}))); if (!short_range) check(graph.mark_output(output));
        auto plan=take(graph.compile()); const std::array external{ExternalBinding{1,nullptr,&image,{}}};
        const std::array callback{PassCallback{0,[](PassContext& pass,void*){return pass.copy_to_buffer(1,0,{0,0,0,0,3,2});}}};
        const auto submitted=queue.stats().submitted;
        auto execution=execute(plan,queue,{external,callback,{}});
        if (short_range) require(!execution && queue.stats().submitted==submitted,"Graph allowed four-byte HDR range");
        else {
            require(bool(execution),"Graph HDR copy failed"); require(take(queue.wait(execution->submission())),"Graph HDR wait failed");
            check(take(execution->buffer(0))->read(0,std::as_writable_bytes(std::span{actual}))); require(actual==values,"Graph HDR bytes differ");
        }
    }
    auto depth=take(queue.create_image({2,2,vk::Format::eD32Sfloat,vk::ImageUsageFlagBits::eTransferDst}));
    auto clear=take(queue.begin()); const auto range=vk::ImageSubresourceRange{vk::ImageAspectFlagBits::eDepth,0,1,0,1};
    const std::array uses{image_use(depth,vk::PipelineStageFlagBits2::eClear,vk::AccessFlagBits2::eTransferWrite,vk::ImageLayout::eTransferDstOptimal,range)};
    check(clear.prepare(uses)); require(!clear.clear_depth(depth,std::numeric_limits<float>::quiet_NaN(),range),"NaN depth clear accepted");
    require(!clear.clear_depth(depth,1,{vk::ImageAspectFlagBits::eColor,0,1,0,1}),"wrong clear aspect accepted");
    check(clear.clear_depth(depth,1,range)); require(take(queue.wait(take(queue.submit(std::move(clear))))),"depth clear wait failed");
    require(take(depth.state()).initialized,"depth clear did not initialize contents");
}
void budget(memory::MemorySystem& system,memory::ResourceHandle asset_heap,SubmissionQueue& queue) {
    Program program; auto cache=take(GpuAssets::create(asset_heap)); program.upload(cache,queue);
    auto view=program.view(asset_heap); auto heap=take(system.create_heap({"pipeline-budget",memory::DomainCategory::render,524288}));
    {
        auto pipeline=take(ScenePipeline::create(heap,queue,DK_RENDER_SHADER_DIR));
        auto warm=take(pipeline.render(queue,view,cache)); require(take(warm.wait(queue)),"budget warmup failed"); warm={};
        const auto base=allocations(queue);
        struct Block{void* pointer;std::size_t size;}; std::vector<Block> blocks;
        while (auto b=heap.try_allocate(2048)) blocks.push_back({*b,2048});
        while (auto b=heap.try_allocate(1)) blocks.push_back({*b,1});
        bool failed=false,partial=false,success=false;
        do {
            auto before=heap.snapshot(); const auto submitted=queue.stats().submitted;
            {
                auto result=pipeline.render(queue,view,cache,{Vec3f::Zero(),1,true});
                if (!result) {
                    failed=true; partial |= heap.snapshot().allocation_count>before.allocation_count+8;
                    require(heap.snapshot().live_allocations==before.live_allocations && allocations(queue)==base && queue.stats().submitted==submitted,"frame budget failure leaked or submitted");
                } else { success=true; require(take(result->wait(queue)),"budget result failed"); }
            }
            if (success || blocks.empty()) break;
            auto b=blocks.back(); blocks.pop_back(); heap.deallocate(b.pointer,b.size);
        } while (true);
        for (auto b:blocks) heap.deallocate(b.pointer,b.size);
        require(failed && partial && success,"frame budget sweep incomplete");
        close_at_submit=heap; auto closing=take(pipeline.render(queue,view,cache,{Vec3f::Zero(),1,true})); close_at_submit={};
        require(heap.state()==memory::ResourceState::closing && !pipeline.render(queue,view,cache),"closing pipeline accepted another frame");
        require(take(closing.wait(queue)),"closing frame wait failed"); expect_image(closing,{});
    }
    require(heap.snapshot().live_allocations==0,"frame/pipeline metadata leaked after close"); cache.clear(); require(allocations(queue)==0,"budget case GPU leak");
}
}
int main() {
    Diagnostics diagnostics; auto system=take(memory::MemorySystem::create()); auto heap=take(system.create_heap({"render-pipeline",memory::DomainCategory::render}));
    DeviceOptions options; options.validation=ValidationMode::required; options.diagnostic_sink=diagnostic; options.diagnostic_user_data=&diagnostics;
    auto device=Device::create(heap,options);
    if (!device) { std::fprintf(stderr,"%s\n",device.error().message.c_str()); return device.error().code==ErrorCode::not_found || device.error().code==ErrorCode::not_supported ? 77 : 1; }
    try {
        {
            memory::ThreadContext context{system}; memory::ExecutionScope scope{context,heap};
            native_submit=reinterpret_cast<PFN_vkQueueSubmit2>(device->device_proc("vkQueueSubmit2"));
            native_wait=reinterpret_cast<PFN_vkWaitSemaphores>(device->device_proc("vkWaitSemaphores"));
            auto queue=take(graphics::detail::SubmissionAccess::create(heap,std::move(*device),3,{submit_override,nullptr,wait_override}));
            std::printf("GPU=%s driver=%s\n",queue.device().adapter().properties.deviceName.data(),queue.device().adapter().driver.driverInfo.data());
            hdr_transfers(heap,queue); require(allocations(queue)==0,"HDR tests leaked");
            render_scenes(heap,queue,options); budget(system,heap,queue);
            // Dropping every public owner before completion leaves pending command retention in charge.
            Program program; auto cache=take(GpuAssets::create(heap)); program.upload(cache,queue);
            auto pipeline=take(ScenePipeline::create(heap,queue,DK_RENDER_SHADER_DIR));
            auto pending=take(pipeline.render(queue,program.view(heap),cache));
            pending={}; pipeline={}; cache.clear(); check(queue.close());
            require(queue.stats().pending_slots==0 && allocations(queue)==0,"pending destruction/close leaked");
            std::printf("pipeline image cases=5 pending=%u allocations=%u\n",queue.stats().pending_slots,allocations(queue));
        }
        device=std::unexpected(Error{ErrorCode::invalid_state,"released"});
        std::printf("errors=%u warnings=%u liveAllocations=%zu\n",diagnostics.errors.load(),diagnostics.warnings.load(),heap.snapshot().live_allocations);
        return diagnostics.errors==0 && diagnostics.warnings==0 && heap.snapshot().live_allocations==0 && system.try_close().closed() ? 0 : 1;
    } catch (const std::exception& error) { std::fprintf(stderr,"render pipeline probe failed: %s\n",error.what()); return 1; }
}

#include <dk/graphics/GraphExecution.hpp>
#include <dk/graphics/ShaderCompiler.hpp>
#include <dk/graphics/Transfer.hpp>
#include <dk/memory/MemorySystem.hpp>
#include "SubmissionInternal.hpp"
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>

using namespace dk;
using namespace dk::graphics;
using namespace dk::graphics::graph;
namespace {
template<class T, class E> T take(std::expected<T,E> result) {
    if (!result) {
        if constexpr (std::is_same_v<E,Error>) throw std::runtime_error(result.error().message);
        else throw std::bad_alloc{};
    }
    return std::move(*result);
}
void check(Result<void> result) { if (!result) throw std::runtime_error(result.error().message); }
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
struct Diagnostics { std::atomic<unsigned> errors=0, warnings=0, known=0; };
void diagnostic(void* context, const Diagnostic& message) noexcept {
    auto& d=*static_cast<Diagnostics*>(context);
    if (message.severity==VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) ++d.errors;
    if (message.severity==VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        ++d.warnings;
        if (message.name=="Loader Message" && message.message=="Layer VK_LAYER_AMD_switchable_graphics uses API version 1.3 which is older than the application specified API version of 1.4. May cause issues.") ++d.known;
    }
    if (message.severity>=VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
        std::fprintf(stderr,"%.*s: %.*s\n",int(message.name.size()),message.name.data(),int(message.message.size()),message.message.data());
}
PFN_vkQueueSubmit2 native_submit=nullptr;
bool fail_submit=false;
VKAPI_ATTR VkResult VKAPI_CALL submit(VkQueue queue, std::uint32_t count, const VkSubmitInfo2* info, VkFence fence) {
    if (std::exchange(fail_submit,false)) return VK_ERROR_OUT_OF_HOST_MEMORY;
    return native_submit(queue,count,info,fence);
}
std::uint32_t allocations(const SubmissionQueue& queue) {
    VmaTotalStatistics stats{}; vmaCalculateStatistics(queue.device().allocator(),&stats);
    return stats.total.statistics.allocationCount;
}
ImageDesc volume_desc(vk::Format format) {
    ImageDesc desc{4,3,format}; desc.depth=5; desc.type=vk::ImageType::e3D;
    return desc;
}
ImageCopyRegion full_region() { return {0,0,0,0,4,3,0,0,0,0,5}; }
void add(Graph& graph, const char* name, std::initializer_list<Use> uses) {
    static_cast<void>(take(graph.add_pass({name,{uses.begin(),uses.size()}})));
}
struct Program { PipelineLayout layout; ComputePipeline pipeline; };
Program program(memory::ResourceHandle heap, ResourceFactory factory, const char* file, bool full_float=false) {
    const std::array defines{ShaderDefine{"VOLUME_FORMAT",full_float ? "\"rgba32f\"" : "\"rgba16f\""}};
    auto shader=take(compile_shader({std::filesystem::path{DK_VOLUME_SHADER_DIR}/file,"computeMain",ShaderStage::compute,{},defines},heap));
    auto module=take(factory.create_shader(shader));
    const std::array modules{&module};
    auto layout=take(factory.create_pipeline_layout(modules));
    auto pipeline=take(factory.create_compute_pipeline({&module,&layout}));
    return {std::move(layout),std::move(pipeline)};
}
Result<void> dispatch(PassContext& pass, const Program& p, const BindingSet& bindings, std::array<std::uint32_t,3> groups) {
    auto encoder=take(pass.compute()); check(encoder.bind_pipeline(p.pipeline));
    const std::array sets{&bindings}; check(encoder.bind_sets(sets));
    return encoder.dispatch(groups);
}
void copy_roundtrip(SubmissionQueue& queue, vk::Format format) {
    auto desc=volume_desc(format); desc.mip_levels=3;
    auto image=take(queue.create_image(desc));
    const auto stride=color_texel_bytes(format);
    const ImageCopyRegion padded{0,0,0,0,4,3,stride*2,6,4,0,5};
    std::vector<std::byte> bytes(static_cast<std::size_t>(padded.buffer_offset+take(image_copy_bytes(desc,padded))),std::byte{0xee});
    std::vector<std::byte> expected(60*stride),actual(expected.size(),std::byte{0x77});
    for (unsigned z=0;z<5;++z) for (unsigned y=0;y<3;++y) for (unsigned x=0;x<4;++x) for (unsigned k=0;k<stride;++k) {
        const auto value=std::byte(((z*100+y*10+x)*31+k)&255);
        bytes[(2+z*24+y*6+x)*stride+k]=value;
        expected[((z*3+y)*4+x)*stride+k]=value;
    }
    auto batch=take(queue.begin());
    auto bad=full_region(); bad.z=1;
    require(!batch.upload(image,bytes,bad),"out-of-bounds upload accepted");
    require(!take(image.state()).initialized,"rejected upload published state");
    check(batch.upload(image,bytes,padded));
    // Partial z overwrite preserves the already initialized full volume.
    const ImageCopyRegion slice{0,0,0,0,4,3,0,0,0,2,1};
    std::vector<std::byte> replacement(12*stride,std::byte{0x42});
    check(batch.upload(image,replacement,slice));
    std::copy(replacement.begin(),replacement.end(),expected.begin()+24*stride);
    auto request=take(batch.readback(image,full_region()));
    require(request.description().depth==5 && request.description().row_pitch==4*stride && request.description().slice_pitch==12*stride,"volume readback metadata wrong");
    const auto untouched=actual;
    require(!take(request.try_read(actual)) && actual==untouched,"unsubmitted readback changed output");
    const ImageCopyRegion mip{1,0,0,0,2,1,0,0,0,0,2};
    std::vector<std::byte> mip_bytes(4*stride,std::byte{0x35}),mip_actual(mip_bytes.size());
    check(batch.upload(image,mip_bytes,mip)); auto mip_request=take(batch.readback(image,mip));
    const auto ticket=take(queue.submit(std::move(batch)));
    image={}; // All volume/staging owners must survive until completion.
    require(!take(request.try_read(actual)) && actual==untouched,"pending readback changed output");
    require(take(queue.wait(ticket)),"volume copy wait failed");
    require(take(request.try_read(actual)) && actual==expected,"pitched volume copy differs");
    require(take(mip_request.try_read(mip_actual)) && mip_actual==mip_bytes,"3D mip copy differs");

    auto partial=take(queue.create_image(volume_desc(format)));
    { auto incomplete=take(queue.begin()); check(incomplete.upload(partial,replacement,slice));
      require(!take(incomplete.state(partial)).initialized,"partial z initialized the full mip");
      require(!incomplete.readback(partial,full_region()),"uninitialized volume read accepted"); }
    require(!take(partial.state()).initialized,"abandoned partial upload published state");
    { auto source=take(queue.create_buffer({replacement.size(),vk::BufferUsageFlagBits::eTransferSrc,BufferMemory::upload}));
      check(source.write(0,replacement)); auto invalid=take(queue.begin());
      auto promised=image_use(partial,vk::PipelineStageFlagBits2::eCopy,vk::AccessFlagBits2::eTransferWrite,vk::ImageLayout::eTransferDstOptimal);
      promised.full_overwrite=true;
      const std::array uses{buffer_use(source,vk::PipelineStageFlagBits2::eCopy,vk::AccessFlagBits2::eTransferRead),promised};
      check(invalid.prepare(uses)); require(!invalid.copy_to_image(source,partial,slice),"partial z copy fulfilled a false full-overwrite promise"); }

    // Legacy base-level helpers also copy every slice.
    auto legacy=take(queue.create_image(volume_desc(format)));
    auto upload=take(queue.create_buffer({expected.size(),vk::BufferUsageFlagBits::eTransferSrc,BufferMemory::upload}));
    auto output=take(queue.create_buffer({expected.size(),vk::BufferUsageFlagBits::eTransferDst,BufferMemory::readback}));
    check(upload.write(0,expected)); auto legacy_batch=take(queue.begin());
    check(legacy_batch.copy_to_image(upload,legacy)); check(legacy_batch.copy_to_buffer(legacy,output));
    require(take(queue.wait(take(queue.submit(std::move(legacy_batch))))),"legacy volume wait failed");
    check(output.read(0,actual)); require(actual==expected,"legacy copy omitted volume slices");
}
void volume_graph(memory::ResourceHandle heap, SubmissionQueue& queue, vk::Format format) {
    const auto factory=queue.resources();
    auto desc=volume_desc(format); desc.usage|=vk::ImageUsageFlagBits::eStorage; desc.require_linear_filter=true;
    auto volume=take(queue.create_image(desc));
    auto view=take(factory.create_view(volume,{vk::ImageViewType::e3D}));
    require(!factory.create_view(volume,{}),"2D view of volume accepted");
    require(!factory.create_view(volume,{vk::ImageViewType::e3D,{vk::ImageAspectFlagBits::eColor,0,1,1,1}}),"volume array layer view accepted");
    const BufferDesc output_desc{5*16,vk::BufferUsageFlagBits::eStorageBuffer|vk::BufferUsageFlagBits::eTransferSrc};
    auto output=take(queue.create_buffer(output_desc));
    struct Context { Program store,sample; BindingSet store_set,sample_set; bool fail=false; } context;
    context.store=program(heap,factory,"volume-store.slang",format==vk::Format::eR32G32B32A32Sfloat);
    context.sample=program(heap,factory,"volume-sample.slang");
    SamplerDesc sampler_desc; sampler_desc.address_u=sampler_desc.address_v=sampler_desc.address_w=vk::SamplerAddressMode::eClampToEdge;
    auto clamp=take(factory.create_sampler(sampler_desc)); sampler_desc.address_w=vk::SamplerAddressMode::eRepeat;
    auto repeat=take(factory.create_sampler(sampler_desc));
    const std::array store_writes{BindingWrite{0,0,ImageBinding{&view,vk::ImageLayout::eGeneral}}};
    context.store_set=take(factory.create_bindings(context.store.layout,0,store_writes));
    std::array sample_writes{BindingWrite{0,0,ImageBinding{&view}},BindingWrite{1,0,SamplerBinding{&clamp}},
        BindingWrite{2,0,SamplerBinding{&repeat}},BindingWrite{3,0,BufferBinding{&output}}};
    context.sample_set=take(factory.create_bindings(context.sample.layout,0,sample_writes));
    { auto flat_desc=desc; flat_desc.depth=1; flat_desc.type=vk::ImageType::e2D;
      auto flat=take(queue.create_image(flat_desc)); auto flat_view=take(factory.create_view(flat,{}));
      sample_writes[0].resource=ImageBinding{&flat_view};
      require(!factory.create_bindings(context.sample.layout,0,sample_writes),"Texture3D accepted Texture2D view"); }
    auto graph=take(Graph::create(heap));
    const auto id=take(graph.declare_image("volume",desc,Lifetime::external));
    const auto result=take(graph.declare_buffer("samples",output_desc,Lifetime::external));
    add(graph,"store volume",{{id,{{vk::PipelineStageFlagBits2::eComputeShader,vk::AccessFlagBits2::eShaderStorageWrite,vk::ImageLayout::eGeneral},0,VK_WHOLE_SIZE,{vk::ImageAspectFlagBits::eColor,0,1,0,1},true}}});
    add(graph,"sample volume",{{id,{{vk::PipelineStageFlagBits2::eComputeShader,vk::AccessFlagBits2::eShaderSampledRead,vk::ImageLayout::eShaderReadOnlyOptimal}}},
        {result,{{vk::PipelineStageFlagBits2::eComputeShader,vk::AccessFlagBits2::eShaderStorageWrite},0,VK_WHOLE_SIZE,{},true}}});
    check(graph.mark_output(id)); check(graph.mark_output(result));
    auto plan=take(graph.compile());
    const std::array callbacks{
        PassCallback{0,[](PassContext& pass,void* data) { auto& c=*static_cast<Context*>(data); return dispatch(pass,c.store,c.store_set,{4,3,5}); },&context},
        PassCallback{1,[](PassContext& pass,void* data)->Result<void> {
            auto& c=*static_cast<Context*>(data);
            if(c.fail) return std::unexpected(Error{ErrorCode::conflict,"injected after volume store"});
            return dispatch(pass,c.sample,c.sample_set,{1,1,1}); },&context}};
    const std::array bindings{ExternalBinding{0,&output},ExternalBinding{1,nullptr,&volume}};
    { auto mismatch_desc=desc; mismatch_desc.depth=1;
      auto mismatch=take(queue.create_image(mismatch_desc));
      std::array wrong{ExternalBinding{0,&output},ExternalBinding{1,nullptr,&mismatch}};
      require(!execute(plan,queue,{wrong,callbacks,{}}),"external volume depth mismatch accepted");
      mismatch_desc=desc; mismatch_desc.require_linear_filter=false; mismatch=take(queue.create_image(mismatch_desc));
      require(!execute(plan,queue,{wrong,callbacks,{}}),"external filter contract mismatch accepted"); }
    const auto before=take(volume.state()); const auto submitted=queue.stats().submitted; const auto live=allocations(queue);
    context.fail=true;
    const auto failed_record=execute(plan,queue,{bindings,callbacks,{},true});
    require(!failed_record && failed_record.error().message=="injected after volume store","record failure did not reach volume callback"); context.fail=false;
    fail_submit=true; require(!execute(plan,queue,{bindings,callbacks,{},true}) && !fail_submit,"submit failure was not injected");
    require(take(volume.state())==before && queue.stats().submitted==submitted && allocations(queue)==live && !queue.stats().recording_slots,"failed volume graph published state or leaked");
    auto execution=take(execute(plan,queue,{bindings,callbacks,{},true}));
    bool barrier=false;
    for(const auto& sync:execution.synchronization()) if(sync.resource==1 && sync.pass==1)
        barrier=sync.before.layout==vk::ImageLayout::eGeneral && bool(sync.before.access&vk::AccessFlagBits2::eShaderStorageWrite) &&
            sync.target.layout==vk::ImageLayout::eShaderReadOnlyOptimal && bool(sync.target.access&vk::AccessFlagBits2::eShaderSampledRead);
    require(barrier,"missing storage-to-sample barrier");
    auto read=take(queue.begin()); auto samples=take(read.readback(output)); auto voxels=take(read.readback(volume,full_region()));
    const auto ticket=take(queue.submit(std::move(read)));
    context={}; view={}; clamp={}; repeat={}; execution={}; volume={}; output={};
    require(take(queue.wait(ticket)),"volume shader wait failed");
    std::array<std::array<float,4>,5> actual{};
    require(take(samples.try_read(std::as_writable_bytes(std::span{actual}))),"samples unavailable");
    const std::array<std::array<float,4>,5> expected{{{0,0,0,1},{3,2,4,1},{1.5f,0.75f,2.25f,1},{0,0,4,1},{0,0,1,1}}};
    for(unsigned i=0;i<5;++i) for(unsigned c=0;c<4;++c)
        require(std::isfinite(actual[i][c]) && std::abs(actual[i][c]-expected[i][c])<0.002f,"3D coordinate/filter/address mismatch");
    std::vector<std::byte> raw(static_cast<std::size_t>(voxels.description().bytes)); require(take(voxels.try_read(raw)),"voxel readback unavailable");
    const std::array<std::uint16_t,5> half{0,0x3c00,0x4000,0x4200,0x4400};
    for(unsigned z=0;z<5;++z) for(unsigned y=0;y<3;++y) for(unsigned x=0;x<4;++x) {
        const std::array values{x,y,z,1u};
        for(unsigned c=0;c<4;++c) {
            const auto index=(((z*3+y)*4+x)*4+c);
            if(format==vk::Format::eR16G16B16A16Sfloat) { std::uint16_t value; std::memcpy(&value,raw.data()+index*2,2); require(value==half[values[c]],"RGBA16F storage coordinates differ"); }
            else { float value; std::memcpy(&value,raw.data()+index*4,4); require(value==static_cast<float>(values[c]),"RGBA32F storage coordinates differ"); }
        }
    }
}
void depth_graph(memory::ResourceHandle heap, SubmissionQueue& queue) {
    const auto factory=queue.resources();
    const ImageDesc desc{4,4,vk::Format::eD32Sfloat,vk::ImageUsageFlagBits::eDepthStencilAttachment|vk::ImageUsageFlagBits::eSampled|vk::ImageUsageFlagBits::eTransferDst};
    auto depth=take(queue.create_image(desc));
    auto view=take(factory.create_view(depth,{vk::ImageViewType::e2D,{vk::ImageAspectFlagBits::eDepth,0,1,0,1}}));
    const BufferDesc output_desc{16*sizeof(float),vk::BufferUsageFlagBits::eStorageBuffer|vk::BufferUsageFlagBits::eTransferSrc};
    auto output=take(queue.create_buffer(output_desc));
    auto shader=program(heap,factory,"depth-sample.slang");
    SamplerDesc sampler_desc; sampler_desc.min_filter=sampler_desc.mag_filter=vk::Filter::eNearest; sampler_desc.mipmap_mode=vk::SamplerMipmapMode::eNearest;
    auto sampler=take(factory.create_sampler(sampler_desc));
    const std::array writes{BindingWrite{0,0,ImageBinding{&view}},BindingWrite{1,0,SamplerBinding{&sampler}},BindingWrite{2,0,BufferBinding{&output}}};
    auto set=take(factory.create_bindings(shader.layout,0,writes));
    auto graph=take(Graph::create(heap));
    const auto id=take(graph.declare_image("depth",desc,Lifetime::external));
    const auto result=take(graph.declare_buffer("depth samples",output_desc,Lifetime::external));
    const vk::ImageSubresourceRange range{vk::ImageAspectFlagBits::eDepth,0,1,0,1};
    add(graph,"initialize depth",{{id,{{vk::PipelineStageFlagBits2::eClear,vk::AccessFlagBits2::eTransferWrite,vk::ImageLayout::eTransferDstOptimal},0,VK_WHOLE_SIZE,range,true}}});
    add(graph,"depth attachment",{{id,{{vk::PipelineStageFlagBits2::eEarlyFragmentTests|vk::PipelineStageFlagBits2::eLateFragmentTests,
        vk::AccessFlagBits2::eDepthStencilAttachmentRead|vk::AccessFlagBits2::eDepthStencilAttachmentWrite,vk::ImageLayout::eDepthStencilAttachmentOptimal},0,VK_WHOLE_SIZE,range}}});
    add(graph,"depth sample",{{id,{{vk::PipelineStageFlagBits2::eComputeShader,vk::AccessFlagBits2::eShaderSampledRead,vk::ImageLayout::eShaderReadOnlyOptimal},0,VK_WHOLE_SIZE,range}},
        {result,{{vk::PipelineStageFlagBits2::eComputeShader,vk::AccessFlagBits2::eShaderStorageWrite},0,VK_WHOLE_SIZE,{},true}}});
    check(graph.mark_output(result)); auto plan=take(graph.compile());
    struct Context { const ImageView* view; const Program* shader; const BindingSet* set; } context{&view,&shader,&set};
    const std::array callbacks{
        PassCallback{0,[](PassContext& pass,void*) { return pass.clear_depth(1,0.875f,{vk::ImageAspectFlagBits::eDepth,0,1,0,1}); }},
        PassCallback{1,[](PassContext& pass,void* data)->Result<void> {
            auto& c=*static_cast<Context*>(data); RenderingDesc rendering; rendering.depth.view=c.view; rendering.depth.clear=0.375f;
            auto encoder=take(pass.begin_rendering(rendering)); return encoder.end(); },&context},
        PassCallback{2,[](PassContext& pass,void* data) { auto& c=*static_cast<Context*>(data); return dispatch(pass,*c.shader,*c.set,{4,4,1}); },&context}};
    const std::array bindings{ExternalBinding{0,&output},ExternalBinding{1,nullptr,&depth}};
    auto execution=take(execute(plan,queue,{bindings,callbacks,{},true}));
    bool barrier=false;
    for(const auto& sync:execution.synchronization()) if(sync.resource==1 && sync.pass==2)
        barrier=sync.before.layout==vk::ImageLayout::eDepthStencilAttachmentOptimal && bool(sync.before.access&vk::AccessFlagBits2::eDepthStencilAttachmentWrite) &&
            sync.target.layout==vk::ImageLayout::eShaderReadOnlyOptimal && bool(sync.target.access&vk::AccessFlagBits2::eShaderSampledRead);
    require(barrier,"missing depth-attachment-to-sample barrier");
    auto batch=take(queue.begin()); auto request=take(batch.readback(output)); const auto ticket=take(queue.submit(std::move(batch)));
    shader={}; set={}; view={}; sampler={}; depth={}; output={}; execution={};
    require(take(queue.wait(ticket)),"depth sample wait failed");
    std::array<float,16> values{}; require(take(request.try_read(std::as_writable_bytes(std::span{values}))),"depth readback unavailable");
    for(auto value:values) require(value==0.375f,"depth sampled value differs");
}
void graph_copy_ranges(memory::ResourceHandle heap, SubmissionQueue& queue) {
    const auto desc=volume_desc(vk::Format::eR16G16B16A16Sfloat);
    auto image=take(queue.create_image(desc));
    const ImageCopyRegion region{0,0,0,0,4,3,16,6,4,0,5};
    const BufferDesc source_desc{region.buffer_offset+take(image_copy_bytes(desc,region)),vk::BufferUsageFlagBits::eTransferSrc,BufferMemory::upload};
    auto source=take(queue.create_buffer(source_desc)); std::vector<std::byte> bytes(static_cast<std::size_t>(source_desc.size),std::byte{0x55}); check(source.write(0,bytes));
    for(bool short_range:{true,false}) {
        auto graph=take(Graph::create(heap));
        const auto src=take(graph.declare_buffer("padded upload",source_desc,Lifetime::external,true));
        const auto dst=take(graph.declare_image("volume",desc,Lifetime::external));
        add(graph,"copy",{{src,{{vk::PipelineStageFlagBits2::eCopy,vk::AccessFlagBits2::eTransferRead},0,short_range ? 16+16*8 : source_desc.size}},
            {dst,{{vk::PipelineStageFlagBits2::eCopy,vk::AccessFlagBits2::eTransferWrite,vk::ImageLayout::eTransferDstOptimal},0,VK_WHOLE_SIZE,{vk::ImageAspectFlagBits::eColor,0,1,0,1},true}}});
        check(graph.mark_output(dst)); const auto plan=take(graph.compile());
        auto copy=region;
        const std::array callbacks{PassCallback{0,[](PassContext& pass,void* data){ return pass.copy_to_image(0,1,*static_cast<ImageCopyRegion*>(data)); },&copy}};
        const std::array bindings{ExternalBinding{0,&source},ExternalBinding{1,nullptr,&image}};
        auto execution=execute(plan,queue,{bindings,callbacks,{}});
        if(short_range) require(!execution && !take(image.state()).initialized,"Graph accepted undeclared z slices");
        else { require(bool(execution),"Graph rejected full volume footprint"); require(take(queue.wait(execution->submission())),"Graph copy wait failed"); }
    }
}
void failures_and_limits(SubmissionQueue& queue) {
    auto desc=volume_desc(vk::Format::eR32Uint); desc.require_linear_filter=true;
    auto rejected=queue.create_image(desc); require(!rejected && rejected.error().code==ErrorCode::not_supported,"integer linear-filter capability accepted");
    desc=volume_desc(vk::Format::eR16G16B16A16Sfloat); desc.depth=queue.device().adapter().properties.limits.maxImageDimension3D+1;
    require(!queue.create_image(desc),"hardware 3D extent exceeded");
    desc=volume_desc(vk::Format::eR16G16B16A16Sfloat); desc.depth=1;
    auto image=take(queue.create_image(desc)); auto view=take(queue.resources().create_view(image,{vk::ImageViewType::e3D}));
    require(!queue.resources().create_view(image,{}),"depth-one 3D treated as 2D");
    ReadbackRequest cancelled; std::array<std::byte,96> input{},output{}; output.fill(std::byte{0x55}); const auto before=output;
    { auto batch=take(queue.begin()); check(batch.upload(image,input,{0,0,0,0,4,3})); cancelled=take(batch.readback(image,{0,0,0,0,4,3}));
      fail_submit=true; require(!queue.submit(std::move(batch)),"failed volume transfer submitted"); }
    require(!cancelled.try_read(output) && before==output && !take(image.state()).initialized,"failed volume readback published state/output");
}
}
int main() {
    Diagnostics diagnostics;
    auto memory=take(memory::MemorySystem::create()); auto heap=take(memory.create_heap({"volume-images",memory::DomainCategory::render}));
    try {
        {
            DeviceOptions options; options.validation=ValidationMode::required; options.diagnostic_sink=diagnostic; options.diagnostic_user_data=&diagnostics;
            auto device=take(Device::create(heap,options)); // Required hardware: unavailable is a failure, never a successful skip.
            native_submit=reinterpret_cast<PFN_vkQueueSubmit2>(device.device_proc("vkQueueSubmit2"));
            auto queue=take(graphics::detail::SubmissionAccess::create(heap,std::move(device),3,{submit}));
            failures_and_limits(queue);
            for(auto format:{vk::Format::eR16G16B16A16Sfloat,vk::Format::eR32G32B32A32Sfloat}) { copy_roundtrip(queue,format); volume_graph(heap,queue,format); }
            depth_graph(heap,queue); graph_copy_ranges(heap,queue);
            require(!queue.stats().pending_slots && !queue.stats().recording_slots && allocations(queue)==0,"volume resources leaked");
            check(queue.close());
        }
        std::printf("volume: RGBA16F/RGBA32F store/sample/filter/address/copy/mip, depth sampling, Graph barriers/ranges, failures and lifetime passed\n");
        std::printf("validation errors=%u warnings=%u known=%u live=%zu\n",diagnostics.errors.load(),diagnostics.warnings.load(),diagnostics.known.load(),heap.snapshot().live_allocations);
        return !diagnostics.errors && diagnostics.warnings==diagnostics.known && !heap.snapshot().live_allocations && memory.try_close().closed() ? 0 : 1;
    } catch(const std::exception& error) { std::fprintf(stderr,"volume probe failed: %s\n",error.what()); return 1; }
}

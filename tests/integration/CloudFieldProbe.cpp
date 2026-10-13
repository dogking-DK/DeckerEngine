#include <dk/render/CloudTextures.hpp>
#include <dk/graphics/ShaderCompiler.hpp>
#include <dk/graphics/Transfer.hpp>
#include <dk/memory/MemorySystem.hpp>
#include "SubmissionInternal.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>

using namespace dk;
using namespace dk::graphics;
using namespace dk::render;
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
void verify_samples(memory::ResourceHandle heap,SubmissionQueue& queue,const CloudTextures& gpu,const CloudFieldData& cpu) {
    const auto factory=queue.resources();
    auto shader=take(compile_shader({std::filesystem::path{DK_CLOUD_SHADER_DIR}/"cloud-fields-sample.slang","computeMain",ShaderStage::compute},heap));
    auto module=take(factory.create_shader(shader)); const std::array modules{&module};
    auto layout=take(factory.create_pipeline_layout(modules)); auto pipeline=take(factory.create_compute_pipeline({&module,&layout}));
    const std::array<std::array<float,4>,8> points{{{.5f/128,.5f/128,.5f/128,0},{127.5f/128,127.5f/128,127.5f/128,0},
        {.37f,.23f,.81f,0},{-.17f,2.39f,-3.61f,0},{0,0,0,0},{1,1,1,0},{.5f/32,.5f/32,.5f/32,0},{1.f/128,1.f/128,1.f/128,0}}};
    auto input=take(queue.create_buffer({sizeof(points),vk::BufferUsageFlagBits::eStorageBuffer|vk::BufferUsageFlagBits::eTransferDst}));
    auto output=take(queue.create_buffer({sizeof(points),vk::BufferUsageFlagBits::eStorageBuffer|vk::BufferUsageFlagBits::eTransferSrc}));
    const std::array writes{BindingWrite{0,0,ImageBinding{&gpu.view(CloudField::weather)}},
        BindingWrite{1,0,ImageBinding{&gpu.view(CloudField::base)}},BindingWrite{2,0,ImageBinding{&gpu.view(CloudField::detail)}},
        BindingWrite{3,0,SamplerBinding{&gpu.sampler()}},BindingWrite{4,0,BufferBinding{&input}},BindingWrite{5,0,BufferBinding{&output}}};
    auto bindings=take(factory.create_bindings(layout,0,writes));
    auto batch=take(queue.begin()); check(batch.upload(input,std::as_bytes(std::span{points})));
    std::array<ResourceUse,5> uses;
    for(unsigned i=0;i<3;++i) uses[i]=image_use(gpu.image(static_cast<CloudField>(i)),vk::PipelineStageFlagBits2::eComputeShader,
        vk::AccessFlagBits2::eShaderSampledRead,vk::ImageLayout::eShaderReadOnlyOptimal);
    uses[3]=buffer_use(input,vk::PipelineStageFlagBits2::eComputeShader,vk::AccessFlagBits2::eShaderStorageRead);
    uses[4]=buffer_use(output,vk::PipelineStageFlagBits2::eComputeShader,vk::AccessFlagBits2::eShaderStorageWrite); uses[4].full_overwrite=true;
    check(batch.prepare(uses));
    auto encoder=take(batch.compute()); check(encoder.bind_pipeline(pipeline)); const std::array sets{&bindings};
    check(encoder.bind_sets(sets)); check(encoder.dispatch({8,1,1})); check(batch.finish_pass());
    auto read=take(batch.readback(output));
    std::array<ReadbackRequest,3> images;
    const std::array sizes{cpu.key().weather_resolution,cpu.key().base_resolution,cpu.key().detail_resolution};
    for(unsigned i=0;i<3;++i) images[i]=take(batch.readback(gpu.image(static_cast<CloudField>(i)),{0,0,0,0,sizes[i],sizes[i],0,0,0,0,i==0 ? 1 : sizes[i]}));
    const auto submission=take(queue.submit(std::move(batch)));
    input={}; output={}; bindings={}; pipeline={}; layout={}; module={};
    require(take(queue.wait(submission)),"cloud sample wait failed");
    std::array<std::array<float,4>,8> actual{}; require(take(read.try_read(std::as_writable_bytes(std::span{actual}))),"sample readback unavailable");
    double maximum=0;
    for(unsigned i=0;i<points.size();++i) {
        EnvironmentVector p{points[i][0],points[i][1],points[i][2]};
        const auto weather=take(cpu.sample(CloudField::weather,p)); const auto base=take(cpu.sample(CloudField::base,p)); const auto detail=take(cpu.sample(CloudField::detail,p));
        const std::array expected{weather[0],weather[1],base[0],detail[0]};
        for(unsigned c=0;c<4;++c) { maximum=std::max(maximum,std::abs(actual[i][c]-expected[c]));
            require(std::isfinite(actual[i][c]) && std::abs(actual[i][c]-expected[c])<=.002,"cloud GPU filter/repeat sample differs from CPU"); }
    }
    for(unsigned i=0;i<3;++i) {
        std::vector<std::byte> bytes(static_cast<std::size_t>(images[i].description().bytes)); require(take(images[i].try_read(bytes)),"texture readback unavailable");
        const auto expected=std::as_bytes(cpu.texels(static_cast<CloudField>(i)));
        require(std::ranges::equal(bytes,expected),"cloud uploaded bytes differ");
    }
    std::printf("cloud fields hash=%016llx bytes=%zu sample_max_abs=%.9f\n",static_cast<unsigned long long>(cpu.hash()),cpu.bytes(),maximum);
}
void run(memory::ResourceHandle heap,SubmissionQueue& queue) {
    CloudParameters c; CloudFieldCache fields;
    auto data=take(fields.prepare(c));
    require(data->hash()==0xc859ef6a9ec30856ULL,"generator-v1 default fixture changed without a version bump");
    auto cache=take(CloudTextureCache::create(queue)); auto first=take(cache->prepare(queue,*data));
    require(first->hash()==data->hash() && first->generation()==1,"cloud metadata mismatch");
    verify_samples(heap,queue,*first,*data);
    const auto submitted=queue.stats().submitted;
    require(take(cache->prepare(queue,*data))==first && queue.stats().submitted==submitted,"cache hit rebuilt textures");
    // Failed second upload must leave generation one and resource counts intact.
    ++c.seed; auto second_data=take(fields.prepare(c));
    const auto live=allocations(queue); fail_submit=true;
    auto failed=cache->prepare(queue,*second_data);
    require(!failed && !fail_submit && cache->current()==first && allocations(queue)==live && !queue.stats().recording_slots,
        "failed upload changed current or leaked resources");
    verify_samples(heap,queue,*first,*data);
    auto second=take(cache->prepare(queue,*second_data)); require(second->generation()==2,"failed upload consumed a generation");
    data.reset(); ++c.seed; auto third_data=take(fields.prepare(c));
    require(!cache->prepare(queue,*third_data) && cache->current()==second,"held old generation did not enforce quota");
    // CPU-independent budget fails before GPU allocation and retains its old generation.
    auto budget=take(CloudTextureCache::create(queue,{2*third_data->bytes(),2}));
    auto budget_first=take(budget->prepare(queue,*third_data)); const auto budget_live=allocations(queue);
    require(!budget->prepare(queue,*second_data) && budget->current()==budget_first && allocations(queue)==budget_live,"texture byte budget failure mutated state");
    // Recording only retains an Image, not the parent CloudTextures. It must still
    // keep the retired generation charged after the last caller snapshot is gone.
    auto recording=take(queue.begin()); check(recording.retain(first->image(CloudField::base)));
    std::weak_ptr<const CloudTextures> retired=first; first.reset();
    require(!cache->prepare(queue,*third_data) && !retired.expired() && cache->current()==second,"recorded retired generation escaped budget");
    recording={}; auto third=take(cache->prepare(queue,*third_data));
    require(third->generation()==3 && retired.expired(),"released old generation did not free capacity");
    verify_samples(heap,queue,*third,*third_data);
    auto tiny=take(CloudTextureCache::create(queue,{1,2})); require(!tiny->prepare(queue,*third_data) && !tiny->current(),"tiny budget accepted textures");
    require(!CloudTextureCache::create(queue,{10000,3}),"unbounded generation limit accepted");
    DeviceOptions options; options.validation=ValidationMode::required;
    auto other=take(SubmissionQueue::create(heap,take(Device::create(heap,options))));
    require(!cache->prepare(other,*third_data) && cache->current()==third,"foreign queue cache hit accepted"); check(other.close());
}
}
int main() {
    Diagnostics diagnostics;
    auto memory=take(memory::MemorySystem::create()); auto heap=take(memory.create_heap({"cloud-fields",memory::DomainCategory::render}));
    try {
        {
            DeviceOptions options; options.validation=ValidationMode::required; options.diagnostic_sink=diagnostic; options.diagnostic_user_data=&diagnostics;
            auto device=take(Device::create(heap,options));
            native_submit=reinterpret_cast<PFN_vkQueueSubmit2>(device.device_proc("vkQueueSubmit2"));
            auto queue=take(graphics::detail::SubmissionAccess::create(heap,std::move(device),3,{submit}));
            run(heap,queue);
            require(!queue.stats().pending_slots && !queue.stats().recording_slots && allocations(queue)==0,"cloud resources leaked");
            check(queue.close());
        }
        std::printf("cloud upload/filter/repeat, budget/old generations, submit failure and foreign queue passed\n");
        std::printf("validation errors=%u warnings=%u known=%u live=%zu\n",diagnostics.errors.load(),diagnostics.warnings.load(),diagnostics.known.load(),heap.snapshot().live_allocations);
        return !diagnostics.errors && diagnostics.warnings==diagnostics.known && !heap.snapshot().live_allocations && memory.try_close().closed() ? 0 : 1;
    } catch(const std::exception& error) { std::fprintf(stderr,"cloud probe failed: %s\n",error.what()); return 1; }
}

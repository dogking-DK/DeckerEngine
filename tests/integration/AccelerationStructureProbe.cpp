#include <dk/graphics/AccelerationStructure.hpp>
#include <dk/memory/MemorySystem.hpp>
#include "AccelerationInternal.hpp"
#include "SubmissionInternal.hpp"
#include <atomic>
#include <cstdio>
#include <stdexcept>

using namespace dk;
using namespace dk::graphics;
namespace {
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
template<class T> T take(Result<T> result) { if (!result) throw std::runtime_error(result.error().message); return std::move(*result); }
void check(Result<void> result) { if (!result) throw std::runtime_error(result.error().message); }
struct Diagnostics { std::atomic<unsigned> errors=0, warnings=0; };
void diagnostic(void* data, const Diagnostic& message) noexcept {
    auto& diagnostics=*static_cast<Diagnostics*>(data);
    if (message.severity==VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) ++diagnostics.errors;
    if (message.severity==VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) ++diagnostics.warnings;
    if (message.severity>=VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
        std::fprintf(stderr,"%.*s: %.*s\n",int(message.name.size()),message.name.data(),int(message.message.size()),message.message.data());
}
PFN_vkQueueSubmit2 native_submit=nullptr;
PFN_vkWaitSemaphores native_wait=nullptr;
VkResult submit_error=VK_SUCCESS;
bool timeout_once=false;
VKAPI_ATTR VkResult VKAPI_CALL submit_override(VkQueue queue, std::uint32_t count, const VkSubmitInfo2* info, VkFence fence) {
    if (submit_error!=VK_SUCCESS) return std::exchange(submit_error,VK_SUCCESS);
    return native_submit(queue,count,info,fence);
}
VKAPI_ATTR VkResult VKAPI_CALL wait_override(VkDevice device, const VkSemaphoreWaitInfo* info, std::uint64_t timeout) {
    if (std::exchange(timeout_once,false)) return VK_TIMEOUT;
    return native_wait(device,info,timeout);
}
constexpr std::array<float,9> triangle{-1,-1,0, 1,-1,0, 0,1,0};
constexpr auto input_usage=vk::BufferUsageFlagBits::eAccelerationStructureBuildInputReadOnlyKHR|vk::BufferUsageFlagBits::eShaderDeviceAddress;
Buffer vertices(const ResourceFactory& factory, bool host=true) {
    auto result=take(factory.create_buffer({sizeof(triangle), input_usage|vk::BufferUsageFlagBits::eTransferDst,
        host ? BufferMemory::upload : BufferMemory::device}));
    if (host) check(result.write(0,std::as_bytes(std::span{triangle})));
    return result;
}
TriangleGeometry geometry(const Buffer& input) {
    TriangleGeometry result; result.vertices=&input; result.vertex_count=3; result.primitive_count=1; return result;
}
std::uint32_t allocations(const Device& device) {
    VmaTotalStatistics stats{}; vmaCalculateStatistics(device.allocator(),&stats); return stats.total.statistics.allocationCount;
}
void run(memory::ResourceHandle heap, Diagnostics& diagnostics) {
    DeviceOptions options; options.validation=ValidationMode::required; options.ray_tracing=RayTracingMode::required;
    options.diagnostic_sink=diagnostic; options.diagnostic_user_data=&diagnostics;
    auto device=take(Device::create(heap,options));
    std::printf("GPU=%s validation=%d scratchAlignment=%u\n",device.adapter().properties.deviceName.data(),device.validation_enabled(),
        device.adapter().ray_tracing.acceleration_structure_limits.minAccelerationStructureScratchOffsetAlignment);
    auto view=take(device.share_queue(0));
    auto foreign_device=take(device.share_queue(0));
    native_submit=reinterpret_cast<PFN_vkQueueSubmit2>(device.device_proc("vkQueueSubmit2"));
    native_wait=reinterpret_cast<PFN_vkWaitSemaphores>(device.device_proc("vkWaitSemaphores"));
    AccelerationStructure survivor;
    ResourceFactory expired;
    std::weak_ptr<graphics::detail::AccelerationState> released;
    {
        auto queue=take(graphics::detail::SubmissionAccess::create(heap,std::move(device),3,{submit_override,nullptr,wait_override}));
        auto foreign=take(SubmissionQueue::create(heap,std::move(foreign_device),1));
        auto factory=queue.resources(); expired=factory;
        auto input=vertices(factory); auto gpu_input=vertices(factory,false);
        const std::array<std::uint16_t,4> indices{0,1,2,0};
        auto index=take(factory.create_buffer({sizeof(indices),input_usage,BufferMemory::upload}));
        check(index.write(0,std::as_bytes(std::span{indices})));
        auto indexed=geometry(input); indexed.indices=&index; indexed.index_type=vk::IndexType::eUint16;
        const std::array<std::uint32_t,3> indices32{0,1,2};
        auto index32=take(factory.create_buffer({sizeof(indices32),input_usage,BufferMemory::upload}));
        check(index32.write(0,std::as_bytes(std::span{indices32})));
        auto indexed32=geometry(input); indexed32.indices=&index32;
        const std::array geometries{geometry(input),indexed,indexed32};
        auto first=take(factory.create_blas(geometries));
        auto second_geometry=geometry(gpu_input);
        auto second=take(factory.create_blas(std::span{&second_geometry,1}));
        const auto address=take(first.device_address());
        require(address && take(first.state()).generation==0 && !take(first.state()).access.initialized,"creation published built state");
        require(first.sizes().storage>0 && first.sizes().scratch_alignment>0,"missing AS sizes");
        auto alias=first.share(); require(take(alias.device_address())==address,"AS share address changed");
        std::array<AccelerationStructureInstance,3> instances{};
        instances[0].blas=&first; instances[1].blas=&second; instances[2].blas=&first;
        instances[1].transform[3]=3; instances[1].custom_index=7;
        instances[2].transform[0]=-1; instances[2].transform[7]=2; instances[2].mask=0x7f;
        auto top=take(factory.create_tlas(instances));
        require(top.kind()==AccelerationStructureKind::top_level,"incorrect TLAS kind");
        const auto baseline=allocations(view);
        require(!factory.create_blas({}) && !factory.create_tlas({}),"empty AS accepted");
        auto bad=second_geometry; bad.vertex_offset=4;
        require(!factory.create_blas(std::span{&bad,1}),"out-of-bounds vertices accepted");
        auto bad_instance=instances[0]; bad_instance.custom_index=0x1000000;
        require(!factory.create_tlas(std::span{&bad_instance,1}),"truncated instance index accepted");
        bad_instance=instances[0]; bad_instance.transform[0]=0;
        require(!factory.create_tlas(std::span{&bad_instance,1}),"singular transform accepted");
        bad_instance=instances[0]; bad_instance.flags=vk::GeometryInstanceFlagsKHR{0x80};
        require(!factory.create_tlas(std::span{&bad_instance,1}),"unknown raw instance flag accepted");
        bad_instance=instances[0]; bad_instance.blas=&top;
        require(!factory.create_tlas(std::span{&bad_instance,1}),"TLAS accepted as BLAS");
        require(!foreign.resources().create_blas(geometries) && !foreign.resources().create_tlas(instances),"foreign submission domain accepted");
        require(allocations(view)==baseline,"validation rejection allocated storage");
        graphics::detail::ObjectAccess::fail_creation(factory,VK_ERROR_OUT_OF_DEVICE_MEMORY);
        require(!factory.create_blas(geometries),"injected AS creation failure ignored");
        require(allocations(view)==baseline,"failed native AS creation leaked storage");
        {
            auto batch=take(queue.begin());
            require(!batch.build(top),"unbuilt BLAS accepted by TLAS");
            require(!batch.build(second),"uninitialized input accepted");
            check(batch.build(first));
            require(!batch.build(first),"same candidate recorded twice");
            require(!input.write(0,std::as_bytes(std::span{triangle})),"recording did not reserve AS input");
            auto competing=take(queue.begin());
            require(!competing.build(first),"candidate reserved by another batch accepted");
            auto wrong=take(foreign.begin()); require(!wrong.build(first),"foreign build accepted");
            require(take(first.state()).generation==0 && !take(first.state()).access.initialized,"recording published candidate");
        }
        check(input.write(0,std::as_bytes(std::span{triangle}))); // Abandon released reservations.
        auto batch=take(queue.begin());
        check(batch.upload(gpu_input,std::as_bytes(std::span{triangle})));
        check(batch.build(first)); check(batch.build(second)); check(batch.build(top)); check(batch.finish_pass());
        auto submission=take(queue.submit(std::move(batch)));
        require(take(first.state()).generation==submission.value() && take(top.state()).status==AccelerationStructureStatus::submitted,
            "submit did not publish AS generation/status");
        const auto pending=queue.stats().retained_resources;
        timeout_once=true; require(!take(queue.wait(submission,0)),"injected timeout ignored");
        require(queue.stats().retained_resources==pending && take(top.state()).status==AccelerationStructureStatus::submitted,
            "timeout released pending state");
        require(!input.write(0,std::as_bytes(std::span{triangle})),"pending input host write accepted");
        require(take(queue.wait(submission)),"AS build did not complete");
        require(take(top.state()).status==AccelerationStructureStatus::completed && queue.stats().retained_resources==0,"completed state not collected");
        require(take(first.device_address())==address,"AS address changed after build");
        { auto rebuild=take(queue.begin()); require(!rebuild.build(first),"in-place AS rebuild accepted"); }
        const auto old_generation=take(top.state()).generation; const auto old_address=take(top.device_address());
        auto replacement=take(factory.create_tlas(instances));
        auto failed=take(queue.begin()); check(failed.build(replacement));
        const auto old_blas_access=take(first.state()).access;
        submit_error=VK_ERROR_OUT_OF_HOST_MEMORY;
        require(!queue.submit(std::move(failed)),"submit failure ignored");
        require(take(replacement.state()).generation==0 && take(replacement.state()).status==AccelerationStructureStatus::candidate,
            "failed submission published AS candidate");
        require(take(top.state()).generation==old_generation && take(top.device_address())==old_address && queue.stats().submitted==submission.value(),
            "failed replacement changed old generation or timeline");
        require(take(first.state()).access==old_blas_access,"failed TLAS submission changed BLAS access ledger");
        auto retry=take(queue.begin()); check(retry.build(replacement));
        auto replacement_submission=take(queue.submit(std::move(retry)));
        top=std::move(replacement); // Application commit: replace only after successful submit.
        require(take(top.state()).generation==replacement_submission.value(),"replacement generation did not advance");
        std::weak_ptr<graphics::detail::AccelerationState> first_owner=graphics::detail::ObjectAccess::state(first);
        std::weak_ptr<graphics::detail::ResourceState> input_owner=graphics::detail::ObjectAccess::state(input);
        first={}; alias={}; second={}; input={}; gpu_input={}; index={}; index32={};
        require(!first_owner.expired(),"TLAS lost referenced BLAS owners");
        require(input_owner.expired(),"completed BLAS retained build-only inputs");
        check(queue.close()); check(foreign.close());
        require(take(top.state()).status==AccelerationStructureStatus::completed,"close did not complete pending TLAS");
        require(!factory.create_tlas(instances),"closed factory accepted creation");
        survivor=std::move(top); released=first_owner;
    }
    require(!expired.create_blas({}),"expired factory accepted creation");
    require(take(survivor.device_address()) && !released.expired(),"AS failed to retain Device/BLAS after queue destruction");
    survivor={}; require(released.expired() && allocations(view)==0,"AS destruction leaked native/VMA owners");
    // Destruction while pending drains the queue after all external AS/input handles are dropped.
    {
        auto queue=take(SubmissionQueue::create(heap,take(view.share_queue(0)),1));
        auto input=vertices(queue.resources()); auto desc=geometry(input);
        auto candidate=take(queue.resources().create_blas(std::span{&desc,1}));
        released=graphics::detail::ObjectAccess::state(candidate);
        AccelerationStructureInstance instance; instance.blas=&candidate;
        auto top=take(queue.resources().create_tlas(std::span{&instance,1}));
        auto batch=take(queue.begin()); check(batch.build(candidate)); check(batch.build(top)); take(queue.submit(std::move(batch)));
        candidate={}; top={}; input={}; require(!released.expired() && queue.stats().pending_slots==1,"pending ownership released early");
    }
    require(released.expired() && allocations(view)==0,"pending destructor did not drain/reclaim AS");
    // DEVICE_LOST is injected before native submit, so no real in-flight work is discarded.
    {
        auto queue=take(graphics::detail::SubmissionAccess::create(heap,take(view.share_queue(0)),1,{submit_override,nullptr,wait_override}));
        auto input=vertices(queue.resources()); auto desc=geometry(input);
        auto candidate=take(queue.resources().create_blas(std::span{&desc,1}));
        auto batch=take(queue.begin()); check(batch.build(candidate)); submit_error=VK_ERROR_DEVICE_LOST;
        require(!queue.submit(std::move(batch)),"lost submit accepted");
        require(take(candidate.state()).status==AccelerationStructureStatus::device_lost && !candidate.device_address(),"lost AS remained usable");
        require(!queue.close() && queue.stats().closed,"lost queue close contract failed");
    }
    require(allocations(view)==0,"lost candidate cleanup leaked VMA allocation");
    // Feature gates apply to AS-specific usages, even without BDA usage in the description.
    options.ray_tracing=RayTracingMode::disabled;
    {
        auto queue=take(SubmissionQueue::create(heap,take(Device::create(heap,options))));
        require(!queue.resources().create_blas({}),"disabled AS factory accepted creation");
        for (const auto usage:{vk::BufferUsageFlagBits::eAccelerationStructureStorageKHR,vk::BufferUsageFlagBits::eAccelerationStructureBuildInputReadOnlyKHR})
            require(!queue.create_buffer({256,usage}),"disabled AS buffer usage accepted");
        check(queue.close());
    }
    std::printf("multiGeometry=3 indices=none/u16/u32 multiInstance=3 sameBatch/crossBatch=pass failures/timeout=pass abandon/pending/close=pass\n");
}
}
int main() {
    try {
        auto system=memory::MemorySystem::create().value(); auto heap=system.create_heap({"as-probe",memory::DomainCategory::render}).value();
        Diagnostics diagnostics; run(heap,diagnostics);
        std::printf("errors=%u warnings=%u liveAllocations=%zu\n",diagnostics.errors.load(),diagnostics.warnings.load(),heap.snapshot().live_allocations);
        require(diagnostics.errors==0 && diagnostics.warnings==0 && heap.snapshot().live_allocations==0,"validation diagnostics or CPU allocations remained");
        heap.begin_close(); require(system.try_close().closed(),"Memory system failed to close"); return 0;
    } catch (const std::exception& error) { std::fprintf(stderr,"required AS probe: %s\n",error.what()); return 1; }
}

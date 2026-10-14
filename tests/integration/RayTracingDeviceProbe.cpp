#include <dk/graphics/Resources.hpp>
#include <dk/memory/MemorySystem.hpp>
#include <atomic>
#include <cstdio>
#include <stdexcept>

using namespace dk;
using namespace dk::graphics;
namespace {
struct Diagnostics { std::atomic<unsigned> errors=0, warnings=0; };
void diagnostic(void* data,const Diagnostic& message) noexcept {
    auto& state=*static_cast<Diagnostics*>(data);
    if (message.severity==VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) ++state.errors;
    if (message.severity==VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) ++state.warnings;
    if (message.severity>=VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
        std::fprintf(stderr,"%.*s: %.*s\n",int(message.name.size()),message.name.data(),int(message.message.size()),message.message.data());
}
template<class T> T take(Result<T> result) {
    if (!result) {
        for (const auto& context:result.error().context) std::fprintf(stderr,"%s\n",context.c_str());
        throw std::runtime_error(result.error().message);
    }
    return std::move(*result);
}
void require(bool condition,const char* message) { if (!condition) throw std::runtime_error(message); }
void check(Result<void> result) { if (!result) throw std::runtime_error(result.error().message); }
void round(memory::ResourceHandle heap,Diagnostics& diagnostics,RayTracingMode mode) {
    DeviceOptions options; options.validation=ValidationMode::required; options.ray_tracing=mode;
    options.diagnostic_sink=diagnostic; options.diagnostic_user_data=&diagnostics; options.secondary_queue=true;
    auto device=take(Device::create(heap,options));
    const auto& rt=device.adapter().ray_tracing;
    const bool enabled=device.ray_tracing_enabled();
    require(enabled==(mode!=RayTracingMode::disabled && rt.supported()),"RT request/support/enabled mismatch");
    const auto& a=rt.acceleration_structure_limits; const auto& p=rt.pipeline_limits;
    if (mode==RayTracingMode::required) std::printf("mode=%d GPU=%s API=%u.%u.%u driver=%s (%s) supported=%d enabled=%d queues=%u validation=%d\n",
        int(mode),device.adapter().properties.deviceName.data(),VK_API_VERSION_MAJOR(device.adapter().properties.apiVersion),
        VK_API_VERSION_MINOR(device.adapter().properties.apiVersion),VK_API_VERSION_PATCH(device.adapter().properties.apiVersion),
        device.adapter().driver.driverName.data(),device.adapter().driver.driverInfo.data(),rt.supported(),enabled,
        device.queue_count(),device.validation_enabled());
    else std::printf("mode=%d supported=%d enabled=%d queues=%u validation=%d\n",int(mode),rt.supported(),enabled,device.queue_count(),device.validation_enabled());
    for (const auto reason:rt.missing()) if (!reason.empty()) std::printf("missing=%.*s\n",int(reason.size()),reason.data());
    if (enabled) {
        require(a.maxGeometryCount && a.maxPrimitiveCount && a.maxInstanceCount && a.minAccelerationStructureScratchOffsetAlignment,
            "Missing AS limits");
        require(p.shaderGroupHandleSize && p.shaderGroupHandleAlignment && p.shaderGroupBaseAlignment && p.maxShaderGroupStride &&
            p.maxRayRecursionDepth && p.maxRayDispatchInvocationCount && p.maxRayHitAttributeSize,"Missing RT pipeline limits");
        require(!a.pNext && !p.pNext,"Dangling capability pNext");
        if (mode==RayTracingMode::required) std::printf("AS geometries=%llu primitives=%llu instances=%llu scratchAlignment=%u descriptorSet=%u perStage=%u\n",
            static_cast<unsigned long long>(a.maxGeometryCount),static_cast<unsigned long long>(a.maxPrimitiveCount),
            static_cast<unsigned long long>(a.maxInstanceCount),a.minAccelerationStructureScratchOffsetAlignment,
            a.maxDescriptorSetAccelerationStructures,a.maxPerStageDescriptorAccelerationStructures);
        if (mode==RayTracingMode::required) std::printf("RT handle=%u handleAlignment=%u baseAlignment=%u stride=%u recursion=%u invocations=%u hitAttribute=%u\n",
            p.shaderGroupHandleSize,p.shaderGroupHandleAlignment,p.shaderGroupBaseAlignment,p.maxShaderGroupStride,
            p.maxRayRecursionDepth,p.maxRayDispatchInvocationCount,p.maxRayHitAttributeSize);
    }
    auto view=take(device.share_queue(device.queue_count()-1));
    require(view.ray_tracing_enabled()==enabled && &view.adapter()==&device.adapter(),"Shared capability state changed");
    Buffer retained; vk::DeviceAddress retained_address=0;
    {
        auto queue=take(SubmissionQueue::create(heap,std::move(device),2));
        auto ordinary=take(queue.create_buffer({256}));
        require(!ordinary.device_address(),"Ordinary buffer exposed address without usage");
        const auto allocations=heap.snapshot().live_allocations;
        const auto usage=vk::BufferUsageFlagBits::eShaderDeviceAddress|vk::BufferUsageFlagBits::eStorageBuffer|vk::BufferUsageFlagBits::eTransferSrc;
        if (!enabled) {
            auto rejected=queue.create_buffer({4096,usage});
            require(!rejected && rejected.error().code==ErrorCode::not_supported,"Disabled BDA allocation accepted");
            require(heap.snapshot().live_allocations==allocations,"Rejected BDA allocation changed live owners");
        } else {
            for (const auto memory:{BufferMemory::device,BufferMemory::upload,BufferMemory::readback}) {
                auto buffer=take(queue.create_buffer({4096,usage,memory}));
                const auto before=take(buffer.state()); const auto address=take(buffer.device_address());
                require(address!=0 && address==take(buffer.device_address()),"Unstable/zero BDA address");
                require(take(buffer.state())==before,"Address query changed access or initialization state");
                auto share=buffer.share(); buffer={}; require(take(share.device_address())==address,"Address changed after owner move/release");
                retained=std::move(share); retained_address=address;
            }
        }
        check(queue.close());
    }
    if (retained) require(take(retained.device_address())==retained_address,"Address owner did not preserve device after queue destruction");
    retained={};
    VmaTotalStatistics stats{}; vmaCalculateStatistics(view.allocator(),&stats);
    require(stats.total.statistics.allocationCount==0,"VMA allocations leaked");
}
}
int main() {
    try {
        auto system=memory::MemorySystem::create().value(); auto heap=system.create_heap({"rt-device-probe",memory::DomainCategory::render}).value();
        Diagnostics diagnostics;
        // Required first: absent RT hardware or validation is a failure, never a skipped acceptance.
        for (const auto mode:{RayTracingMode::required,RayTracingMode::disabled,RayTracingMode::if_available}) round(heap,diagnostics,mode);
        std::printf("errors=%u warnings=%u liveAllocations=%zu\n",diagnostics.errors.load(),diagnostics.warnings.load(),heap.snapshot().live_allocations);
        require(diagnostics.errors==0 && diagnostics.warnings==0 && heap.snapshot().live_allocations==0,"Validation diagnostics or live allocations after teardown");
        heap.begin_close(); require(system.try_close().closed(),"Memory system did not close");
        return 0;
    } catch (const std::exception& e) { std::fprintf(stderr,"required RT device probe: %s\n",e.what()); return 1; }
}

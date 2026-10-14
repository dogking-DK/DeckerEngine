#include "RayTracingInternal.hpp"
#include "PipelinePolicy.hpp"
#include "CommandInternal.hpp"
#include "AccelerationPolicy.hpp"
#include <cstring>

namespace dk::graphics::detail {
namespace {
Error invalid(const char* message) { return {ErrorCode::invalid_argument, message}; }
Result<vk::DeviceSize> aligned(vk::DeviceSize size, vk::DeviceSize alignment) {
    if (!alignment || !std::has_single_bit(alignment) || size > UINT64_MAX-(alignment-1))
        return std::unexpected(invalid("SBT alignment or size overflow"));
    return (size+alignment-1)&~(alignment-1);
}
}
Result<ShaderTableLayout> shader_table_layout(const vk::PhysicalDeviceRayTracingPipelinePropertiesKHR& limits,
    std::uint32_t misses, std::uint32_t hits)
{
    if (!misses || !hits || !limits.shaderGroupHandleSize || std::uint64_t{misses}+hits+1 > UINT32_MAX)
        return std::unexpected(invalid("SBT requires nonempty bounded shader groups"));
    auto stride = aligned(limits.shaderGroupHandleSize, limits.shaderGroupHandleAlignment);
    if (!stride || *stride > limits.maxShaderGroupStride) return std::unexpected(invalid("SBT stride exceeds device limit"));
    auto miss = aligned(*stride, limits.shaderGroupBaseAlignment);
    if (!miss || *stride > (UINT64_MAX-*miss)/misses) return std::unexpected(invalid("SBT miss range overflows"));
    auto hit = aligned(*miss+*stride*misses, limits.shaderGroupBaseAlignment);
    if (!hit || *stride > (UINT64_MAX-*hit)/hits) return std::unexpected(invalid("SBT hit range overflows"));
    const auto bytes = *hit+*stride*hits;
    auto capacity = scratch_capacity(bytes, limits.shaderGroupBaseAlignment);
    if (!capacity) return std::unexpected(capacity.error());
    return ShaderTableLayout{*stride,*miss,*hit,bytes,*capacity};
}
Result<void> ray_dispatch_dimensions(std::array<std::uint32_t,3> dimensions, const vk::PhysicalDeviceLimits& limits,
    std::uint32_t max_invocations)
{
    std::uint64_t product=1;
    for (std::size_t i=0; i<3; ++i) {
        const auto dimension=dimensions[i];
        if (!dimension || dimension > std::uint64_t{limits.maxComputeWorkGroupCount[i]}*limits.maxComputeWorkGroupSize[i] ||
            dimension > max_invocations/product) return std::unexpected(invalid("ray dispatch exceeds dimensions/invocation limits"));
        product *= dimension;
    }
    return {};
}
Result<void> ray_hit_range(std::uint32_t offset, std::uint32_t geometries, std::uint32_t ray_types, std::uint32_t hit_count)
{
    if (!geometries || !ray_types || ray_types>15 || std::uint64_t{offset}+std::uint64_t{geometries}*ray_types > hit_count)
        return std::unexpected(invalid("instance/geometry/ray-type index exceeds SBT hit records"));
    return {};
}
Result<void> ray_payload_compatible(const ShaderRayInterface& caller, const ShaderRayInterface& callee, const ShaderRayTrace& trace)
{
    const auto outgoing=std::ranges::find_if(caller.payloads,[&](const auto& p) { return p.kind==RayInterfaceKind::outgoing_payload && p.location==trace.payload_location; });
    if (outgoing==caller.payloads.end()) return std::unexpected(invalid("TraceRay has no matching outgoing payload"));
    unsigned incoming=0;
    for (const auto& payload : callee.payloads) {
        if (payload.kind==RayInterfaceKind::incoming_payload) {
            ++incoming;
            if (payload.signature!=outgoing->signature || payload.bytes!=outgoing->bytes)
                return std::unexpected(invalid("ray payload structural type mismatch"));
        } else if (payload.kind==RayInterfaceKind::hit_attribute &&
            (payload.bytes!=8 || (payload.signature!="v2(f32)" && payload.signature!="s(v2(f32);)")))
            return std::unexpected(invalid("triangle hit attribute must contain float2 barycentrics"));
    }
    if (incoming>1 || !callee.traces.empty()) return std::unexpected(invalid("only raygen may TraceRay; callee requires at most one incoming payload"));
    return {};
}
} // namespace dk::graphics::detail

namespace dk::graphics {
vk::Pipeline RayTracingPipeline::handle() const noexcept { return state_ ? *state_->pipeline : vk::Pipeline{}; }
ShaderBindingTableRegions ShaderBindingTable::regions() const noexcept { return state_ ? state_->regions : ShaderBindingTableRegions{}; }
Result<AccessState> ShaderBindingTable::state() const {
    if (!state_) return std::unexpected(Error{ErrorCode::invalid_state,"empty SBT"});
    return state_->storage.state();
}
Result<RayTracingPipeline> ResourceFactory::create_ray_tracing_pipeline(const RayTracingPipelineDesc& desc) const
{
    auto checked=detail::factory_queue(queue_); if (!checked) return std::unexpected(checked.error());
    auto& queue=**checked;
    if (!queue.owner->device.ray_tracing_enabled()) return std::unexpected(Error{ErrorCode::not_supported,"RT pipeline requires enabled ray tracing"});
    const auto& rt=queue.owner->device.adapter().ray_tracing.pipeline_limits;
    if (!desc.ray_type_count || desc.ray_type_count>15 || desc.misses.size()!=desc.ray_type_count || desc.hits.empty() ||
        desc.hits.size()%desc.ray_type_count || desc.hits.size()>120 || !rt.maxRayRecursionDepth || rt.maxRayHitAttributeSize<8)
        return std::unexpected(Error{ErrorCode::invalid_argument,"invalid ray type/miss/hit group counts or RT limits"});
    if (auto valid=detail::check_pipeline_stage(queue,desc.raygeneration,desc.layout,ShaderStage::raygeneration); !valid) return std::unexpected(valid.error());
    const auto& layout=detail::ObjectAccess::state(*desc.layout);
    if (layout->push.size()>1) return std::unexpected(Error{ErrorCode::not_supported,"RT requires a single shared push constant range"});
    const auto& ray=detail::ObjectAccess::state(*desc.raygeneration);
    for (const auto* miss : desc.misses)
        if (auto valid=detail::check_pipeline_stage(queue,miss,desc.layout,ShaderStage::miss); !valid) return std::unexpected(valid.error());
    for (const auto& hit : desc.hits) {
        if (auto valid=detail::check_pipeline_stage(queue,hit.closest_hit,desc.layout,ShaderStage::closesthit); !valid) return std::unexpected(valid.error());
        if (hit.any_hit) if (auto valid=detail::check_pipeline_stage(queue,hit.any_hit,desc.layout,ShaderStage::anyhit); !valid) return std::unexpected(valid.error());
    }
    for (const auto& trace : ray->ray_interface.traces) {
        if (trace.stride!=desc.ray_type_count || trace.offset>=desc.ray_type_count || trace.miss>=desc.misses.size())
            return std::unexpected(Error{ErrorCode::invalid_argument,"TraceRay selectors do not match ray-type SBT layout"});
        if (auto valid=detail::ray_payload_compatible(ray->ray_interface,detail::ObjectAccess::state(*desc.misses[trace.miss])->ray_interface,trace); !valid)
            return std::unexpected(valid.error());
        for (std::size_t i=trace.offset; i<desc.hits.size(); i+=desc.ray_type_count) {
            for (const auto* hit : {desc.hits[i].closest_hit,desc.hits[i].any_hit}) if (hit)
                if (auto valid=detail::ray_payload_compatible(ray->ray_interface,detail::ObjectAccess::state(*hit)->ray_interface,trace); !valid) return std::unexpected(valid.error());
        }
    }
    auto state=memory::make_shared_in<detail::RayTracingState>(queue.resource,queue.owner,queue.resource,layout);
    state->point=vk::PipelineBindPoint::eRayTracingKHR; state->ray_types=desc.ray_type_count;
    state->miss_count=static_cast<std::uint32_t>(desc.misses.size()); state->hit_count=static_cast<std::uint32_t>(desc.hits.size());
    Vector<vk::PipelineShaderStageCreateInfo> stages{memory::Allocator<vk::PipelineShaderStageCreateInfo>{queue.resource}};
    Vector<vk::RayTracingShaderGroupCreateInfoKHR> groups{memory::Allocator<vk::RayTracingShaderGroupCreateInfoKHR>{queue.resource}};
    const auto stage=[&](const ShaderModule* shader) {
        if (!shader) return VK_SHADER_UNUSED_KHR;
        const auto& value=detail::ObjectAccess::state(*shader);
        const auto index=static_cast<std::uint32_t>(stages.size());
        stages.emplace_back(vk::PipelineShaderStageCreateFlags{},detail::native_stage(value->stage),*value->shader,value->entry.c_str());
        return index;
    };
    groups.emplace_back(vk::RayTracingShaderGroupTypeKHR::eGeneral,stage(desc.raygeneration),VK_SHADER_UNUSED_KHR,VK_SHADER_UNUSED_KHR,VK_SHADER_UNUSED_KHR);
    for (const auto* miss : desc.misses) groups.emplace_back(vk::RayTracingShaderGroupTypeKHR::eGeneral,stage(miss),VK_SHADER_UNUSED_KHR,VK_SHADER_UNUSED_KHR,VK_SHADER_UNUSED_KHR);
    for (const auto& hit : desc.hits) groups.emplace_back(vk::RayTracingShaderGroupTypeKHR::eTrianglesHitGroup,VK_SHADER_UNUSED_KHR,stage(hit.closest_hit),stage(hit.any_hit),VK_SHADER_UNUSED_KHR);
    vk::RayTracingPipelineCreateInfoKHR info{};
    info.setStages(stages).setGroups(groups).setMaxPipelineRayRecursionDepth(1).setLayout(*layout->layout);
    try { state->pipeline=vk::raii::Pipeline{queue.owner->device.logical_device(),nullptr,nullptr,info}; }
    catch (const vk::SystemError& error) { return std::unexpected(queue.failure("create ray tracing pipeline",static_cast<VkResult>(error.code().value()))); }
    if (auto status=detail::object_creation_status(queue,"create ray tracing pipeline"); !status) return std::unexpected(status.error());
    return RayTracingPipeline{std::move(state)};
}
Result<ShaderBindingTable> ResourceFactory::create_shader_binding_table(const RayTracingPipeline& pipeline) const
{
    auto checked=detail::factory_queue(queue_); if (!checked) return std::unexpected(checked.error());
    auto& queue=**checked;
    const auto& owner=detail::ObjectAccess::state(pipeline);
    if (!owner || owner->owner!=queue.owner) return std::unexpected(Error{ErrorCode::invalid_argument,"SBT requires a local RT pipeline"});
    const auto& limits=queue.owner->device.adapter().ray_tracing.pipeline_limits;
    auto layout=detail::shader_table_layout(limits,owner->miss_count,owner->hit_count); if (!layout) return std::unexpected(layout.error());
    auto storage=create_buffer({layout->capacity,vk::BufferUsageFlagBits::eShaderBindingTableKHR|vk::BufferUsageFlagBits::eShaderDeviceAddress,BufferMemory::upload});
    if (!storage) return std::unexpected(storage.error());
    auto base=storage->device_address(); if (!base) return std::unexpected(base.error());
    auto address=detail::scratch_address(*base,storage->size(),layout->bytes,limits.shaderGroupBaseAlignment); if (!address) return std::unexpected(address.error());
    const std::uint32_t count=1+owner->miss_count+owner->hit_count;
    Vector<std::byte> handles(count*std::size_t{limits.shaderGroupHandleSize},std::byte{},memory::Allocator<std::byte>{queue.resource});
    const auto& device=queue.owner->device;
    const auto result=device.logical_device().getDispatcher()->vkGetRayTracingShaderGroupHandlesKHR(device.native_device(),static_cast<VkPipeline>(pipeline.handle()),0,count,handles.size(),handles.data());
    if (result!=VK_SUCCESS) return std::unexpected(queue.failure("get ray tracing shader group handles",result));
    Vector<std::byte> bytes(static_cast<std::size_t>(layout->capacity),std::byte{},memory::Allocator<std::byte>{queue.resource});
    const auto start=*address-*base;
    for (std::uint32_t i=0; i<count; ++i) {
        const auto destination=i==0 ? 0 : i<=owner->miss_count ? layout->miss_offset+(i-1)*layout->stride : layout->hit_offset+(i-1-owner->miss_count)*layout->stride;
        std::memcpy(bytes.data()+start+destination,handles.data()+std::size_t{i}*limits.shaderGroupHandleSize,limits.shaderGroupHandleSize);
    }
    if (auto written=storage->write(0,bytes); !written) return std::unexpected(written.error());
    auto state=memory::make_shared_in<detail::ShaderTableState>(queue.resource,queue.owner);
    state->pipeline=owner; state->storage=std::move(*storage);
    state->regions.raygeneration=vk::StridedDeviceAddressRegionKHR{*address,layout->stride,layout->stride};
    state->regions.miss=vk::StridedDeviceAddressRegionKHR{*address+layout->miss_offset,layout->stride,layout->stride*owner->miss_count};
    state->regions.hit=vk::StridedDeviceAddressRegionKHR{*address+layout->hit_offset,layout->stride,layout->stride*owner->hit_count};
    if (auto status=detail::object_creation_status(queue,"create shader binding table"); !status) return std::unexpected(status.error());
    return ShaderBindingTable{std::move(state)};
}
} // namespace dk::graphics

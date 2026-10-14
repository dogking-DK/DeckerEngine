#include "RayTracingInternal.hpp"
#include "CommandInternal.hpp"

namespace dk::graphics {
namespace {
using namespace detail;
Error invalid(const char* text) { return {ErrorCode::invalid_argument,text}; }
Error bad_state(const char* text) { return {ErrorCode::invalid_state,text}; }
AccelerationUse* find_as(BatchState& batch,const std::shared_ptr<AccelerationState>& resource) {
    for (auto& use : batch.accelerations) if (use.resource==resource) return &use;
    return nullptr;
}
Result<void> readable_as(BatchState& batch,const std::shared_ptr<AccelerationState>& resource,bool prepared) {
    if (!resource || resource->owner!=batch.queue->owner) return std::unexpected(invalid("trace AS is empty or foreign"));
    const auto* local=find_as(batch,resource);
    if (resource->reserved && !local) return std::unexpected(Error{ErrorCode::conflict,"trace AS is reserved by another batch"});
    const auto access=local ? local->access : resource->state.access;
    if (!access.initialized || (prepared && (!local || !stage_covers(access.stages,vk::PipelineStageFlagBits2::eRayTracingShaderKHR) ||
        !(access.access & vk::AccessFlagBits2::eAccelerationStructureReadKHR))))
        return std::unexpected(bad_state("trace AS requires built content and explicit trace preparation"));
    return {};
}
struct AsRollback {
    BatchState& batch; std::size_t count; bool done=false;
    ~AsRollback() { if (!done) while (batch.accelerations.size()>count) {
        batch.accelerations.back().resource->reserved=false; batch.accelerations.pop_back();
    } }
};
}
Result<void> CommandBatch::prepare_trace(const ShaderBindingTable& table,std::span<const AccelerationStructure* const> structures)
{
    if (!state_) return std::unexpected(bad_state("empty batch"));
    auto& batch=*state_;
    if (auto valid=outside_rendering(batch); !valid) return valid;
    const auto& sbt=ObjectAccess::state(table);
    if (!sbt || sbt->owner!=batch.queue->owner) return std::unexpected(invalid("trace preparation requires a local SBT"));
    Vector<std::shared_ptr<AccelerationState>> resources{memory::Allocator<std::shared_ptr<AccelerationState>>{batch.queue->resource}};
    const auto add=[&](const std::shared_ptr<AccelerationState>& resource) -> Result<void> {
        if (auto valid=readable_as(batch,resource,false); !valid) return valid;
        if (std::find(resources.begin(),resources.end(),resource)==resources.end()) resources.push_back(resource);
        return {};
    };
    for (const auto* structure : structures) {
        if (!structure || !*structure || structure->kind()!=AccelerationStructureKind::top_level) return std::unexpected(invalid("trace preparation requires TLAS owners"));
        const auto& resource=ObjectAccess::state(*structure);
        if (auto valid=add(resource); !valid) return valid;
        for (const auto& child : resource->children) if (auto valid=add(child); !valid) return valid;
    }
    RetainRollback rollback{batch}; AsRollback accelerations{batch,batch.accelerations.size()};
    batch.accelerations.reserve(batch.accelerations.size()+resources.size());
    vk::MemoryBarrier2 memory_barrier{};
    for (const auto& resource : resources) {
        auto* local=find_as(batch,resource);
        if (!local) { batch.accelerations.push_back({resource,resource->state.access,false}); resource->reserved=true; local=&batch.accelerations.back(); }
        memory_barrier.srcStageMask |= local->access.stages; memory_barrier.srcAccessMask |= local->access.access;
    }
    if (auto valid=retain_object(batch,sbt); !valid) return valid;
    const ResolvedUse input{ObjectAccess::state(sbt->storage),{vk::PipelineStageFlagBits2::eRayTracingShaderKHR,vk::AccessFlagBits2::eShaderRead},0,sbt->storage.size(),{},false};
    if (auto valid=prepare_resolved(batch,std::span{&input,1}); !valid) return valid;
    memory_barrier.setDstStageMask(vk::PipelineStageFlagBits2::eRayTracingShaderKHR).setDstAccessMask(vk::AccessFlagBits2::eAccelerationStructureReadKHR);
    if (!resources.empty()) { vk::DependencyInfo dependency{}; dependency.setMemoryBarriers(memory_barrier); batch.command().pipelineBarrier2(dependency); }
    for (const auto& resource : resources) find_as(batch,resource)->access={vk::PipelineStageFlagBits2::eRayTracingShaderKHR,vk::AccessFlagBits2::eAccelerationStructureReadKHR,vk::ImageLayout::eUndefined,true};
    rollback.committed=accelerations.done=true;
    return {};
}
Result<RayTracingEncoder> CommandBatch::ray_tracing()
{
    if (!state_) return std::unexpected(bad_state("empty batch"));
    if (auto valid=outside_rendering(*state_); !valid) return std::unexpected(valid.error());
    if (!state_->queue->owner->device.ray_tracing_enabled()) return std::unexpected(Error{ErrorCode::not_supported,"RT encoder requires enabled ray tracing"});
    invalidate_encoder(*state_);
    return RayTracingEncoder{state_,state_->generation};
}
Result<void> RayTracingEncoder::trace(const RayTracingPipeline& pipeline,const ShaderBindingTable& table,
    std::span<const BindingSet* const> sets,std::span<const std::byte> constants,std::array<std::uint32_t,3> dimensions)
{
    auto checked=batch_.lock();
    if (!checked || checked->generation!=generation_) return std::unexpected(bad_state("trace encoder has ended or expired"));
    auto& batch=*checked;
    if (auto valid=outside_rendering(batch); !valid) return valid;
    const auto& program=ObjectAccess::state(pipeline); const auto& sbt=ObjectAccess::state(table);
    if (!program || program->owner!=batch.queue->owner || !sbt || sbt->pipeline!=program)
        return std::unexpected(invalid("trace requires a local pipeline and its own SBT"));
    const auto& device=batch.queue->owner->device;
    if (auto valid=ray_dispatch_dimensions(dimensions,device.adapter().properties.limits,device.adapter().ray_tracing.pipeline_limits.maxRayDispatchInvocationCount); !valid) return valid;
    if (auto valid=require_use(batch,ObjectAccess::state(sbt->storage),vk::PipelineStageFlagBits2::eRayTracingShaderKHR,vk::AccessFlagBits2::eShaderRead); !valid) return valid;
    if (program->layout->push.empty() ? !constants.empty() : constants.size()!=program->layout->push[0].offset+program->layout->push[0].size)
        return std::unexpected(invalid("trace push constant bytes do not match pipeline layout"));
    Vector<std::shared_ptr<BindingState>> bindings{memory::Allocator<std::shared_ptr<BindingState>>{batch.queue->resource}};
    bindings.resize(program->layout->sets.size());
    for (const auto* input : sets) {
        if (!input || !*input) return std::unexpected(invalid("empty trace binding set"));
        const auto& set=ObjectAccess::state(*input);
        if (!compatible_layouts(*set->layout,*program->layout,set->set_index) || bindings[set->set_index])
            return std::unexpected(invalid("foreign duplicate or incompatible trace bindings"));
        bindings[set->set_index]=set;
    }
    for (const auto& binding : program->layout->bindings) {
        const auto& set=bindings[binding.set];
        if (!set) return std::unexpected(bad_state("trace descriptor set is unset"));
        for (const auto& bound : set->resources) {
            if (bound.binding.binding!=binding.binding || bound.sampler) continue;
            if (bound.acceleration) {
                if (auto valid=readable_as(batch,bound.acceleration,true); !valid) return valid;
                for (const auto& child : bound.acceleration->children) if (auto valid=readable_as(batch,child,true); !valid) return valid;
                for (const auto& range : bound.acceleration->hit_ranges)
                    if (auto valid=ray_hit_range(range[0],range[1],program->ray_types,program->hit_count); !valid) return valid;
                continue;
            }
            const auto resource=bound.buffer ? bound.buffer : bound.view->image;
            const auto* range=bound.view ? &bound.view->description.range : nullptr;
            vk::AccessFlags2 access{};
            if (binding.type==ShaderDescriptorType::uniform_buffer) {
                if (bound.size<binding.minimum_buffer_size) return std::unexpected(invalid("trace uniform binding too small"));
                access=vk::AccessFlagBits2::eUniformRead;
            } else if (binding.type==ShaderDescriptorType::sampled_image) access=vk::AccessFlagBits2::eShaderSampledRead;
            else {
                const auto* use=batch.find(resource);
                if (!use) return std::unexpected(bad_state("trace storage resource requires explicit access preparation"));
                const auto index=range ? static_cast<std::size_t>(range->baseArrayLayer)*resource->image_desc.mip_levels+range->baseMipLevel : 0;
                access=use->states[index].access & (vk::AccessFlagBits2::eShaderRead|vk::AccessFlagBits2::eShaderWrite|vk::AccessFlagBits2::eShaderStorageRead|vk::AccessFlagBits2::eShaderStorageWrite);
                if (!access) return std::unexpected(bad_state("trace storage access has no shader read/write intent"));
            }
            if (auto valid=require_use(batch,resource,vk::PipelineStageFlagBits2::eRayTracingShaderKHR,access,range,bound.image_layout); !valid) return valid;
        }
    }
    RetainRollback rollback{batch};
    if (auto valid=retain_object(batch,sbt); !valid) return valid;
    for (const auto& set : bindings) if (set) {
        if (auto valid=retain_object(batch,set); !valid) return valid;
        for (const auto& bound : set->resources) {
            if (bound.buffer) { if (auto valid=batch.retain(bound.buffer); !valid) return valid; }
            if (bound.view) { if (auto valid=batch.retain(bound.view->image); !valid) return valid; }
        }
    }
    batch.command().bindPipeline(vk::PipelineBindPoint::eRayTracingKHR,*program->pipeline);
    for (const auto& set : bindings) if (set)
        batch.command().bindDescriptorSets(vk::PipelineBindPoint::eRayTracingKHR,*program->layout->layout,set->set_index,*set->set,{});
    if (!constants.empty()) {
        const auto& range=program->layout->push[0];
        batch.command().pushConstants<std::byte>(*program->layout->layout,range.stageFlags,range.offset,constants.subspan(range.offset,range.size));
    }
    const auto& r=sbt->regions;
    batch.command().traceRaysKHR(r.raygeneration,r.miss,r.hit,r.callable,dimensions[0],dimensions[1],dimensions[2]);
    for (const auto& set : bindings) if (set) for (const auto& bound : set->resources) {
        if (bound.buffer) consume_use(batch,bound.buffer);
        if (bound.view) consume_use(batch,bound.view->image,&bound.view->description.range);
    }
    rollback.committed=true;
    return {};
}
} // namespace dk::graphics

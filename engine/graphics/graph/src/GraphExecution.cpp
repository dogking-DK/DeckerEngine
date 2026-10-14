#include <dk/graphics/GraphExecution.hpp>
#include "GraphPlanInternal.hpp"
#include <algorithm>
#include <exception>
#include <dk/profiling/Profiler.hpp>

namespace dk::graphics::graph {
namespace detail {
struct BoundResource {
    Buffer buffer; Image image; AccelerationStructure acceleration; ShaderBindingTable shader_table;
    explicit operator bool() const noexcept { return buffer || image || acceleration || shader_table; }
};
struct ExecutionState {
    ExecutionState(memory::ResourceHandle heap, std::size_t count)
        : resources(count, memory::Allocator<BoundResource>{heap}), states(0, memory::Allocator<ExportedState>{heap}), synchronization(0, memory::Allocator<Synchronization>{heap}) {}
    Vector<BoundResource> resources;
    Vector<ExportedState> states;
    Vector<Synchronization> synchronization;
    Submission submission;
};
struct ExecutionAccess {
    static const auto& state(const CompiledGraph& plan) { return plan.state_; }
    static Execution publish(std::shared_ptr<ExecutionState> state) { return Execution{std::move(state)}; }
    static PassContext context(const CompiledGraph& plan, ExecutionState& state, CommandBatch& batch, std::size_t pass)
    { return PassContext{plan, state, batch, pass}; }
};
} // namespace detail
namespace {
Error invalid(std::string message) { return {ErrorCode::invalid_argument, std::move(message)}; }
Error missing() { return invalid("resource is not a declared resource of this pass or has the wrong type"); }
Error range_error() { return invalid("command range exceeds the pass declaration"); }
bool equal(const BufferDesc& a, const BufferDesc& b) { return a.size == b.size && a.usage == b.usage && a.memory == b.memory; }
bool equal(const ImageDesc& a, const ImageDesc& b) {
    return a.width == b.width && a.height == b.height && a.format == b.format && a.usage == b.usage &&
        a.mip_levels == b.mip_levels && a.array_layers == b.array_layers && a.depth == b.depth &&
        a.type == b.type && a.require_linear_filter == b.require_linear_filter;
}
ResourceUse use_for(detail::BoundResource& resource, const AccessDescription& access) {
    return {resource.buffer ? &resource.buffer : nullptr, resource.image ? &resource.image : nullptr,
        access.state, access.offset, access.size, access.range, access.full_overwrite};
}
Result<void> import_resource(detail::BoundResource& bound, const PlannedResource& resource, const ExternalBinding& binding) {
    if (int(bool(binding.buffer))+int(bool(binding.image))+int(bool(binding.acceleration))+int(bool(binding.shader_table))!=1)
        return std::unexpected(invalid("external binding requires exactly one owner"));
    if (std::holds_alternative<AccelerationDesc>(resource.description) || std::holds_alternative<ShaderTableDesc>(resource.description)) {
        const auto* desc=std::get_if<AccelerationDesc>(&resource.description);
        if ((desc && (!binding.acceleration || !*binding.acceleration || binding.acceleration->kind()!=desc->kind)) ||
            (!desc && (!binding.shader_table || !*binding.shader_table)) || binding.expected_states.size()>1 || (!desc && binding.expected_generation))
            return std::unexpected(invalid("external AS/SBT binding type or expected state mismatch"));
        AccessState live;
        if (desc) {
            auto state=binding.acceleration->state(); if (!state) return std::unexpected(state.error());
            if (binding.expected_generation && *binding.expected_generation!=state->generation)
                return std::unexpected(Error{ErrorCode::conflict,"external AS generation is stale"});
            live=state->access; bound.acceleration=binding.acceleration->share();
        } else {
            auto state=binding.shader_table->state(); if (!state) return std::unexpected(state.error());
            live=*state; bound.shader_table=binding.shader_table->share();
        }
        if ((!binding.expected_states.empty() && live!=binding.expected_states[0]) || (resource.initialized && !live.initialized))
            return std::unexpected(Error{ErrorCode::conflict,"external AS/SBT state or contents is stale"});
        return {};
    }
    if (binding.expected_generation) return std::unexpected(invalid("generation is only supported for AS"));
    const auto* buffer = std::get_if<BufferDesc>(&resource.description);
    if (bool(binding.buffer) == bool(binding.image) || bool(buffer) != bool(binding.buffer))
        return std::unexpected(invalid("external binding type mismatch"));
    if (buffer) {
        if (!*binding.buffer || !equal(*buffer, binding.buffer->description()))
            return std::unexpected(invalid("external buffer description mismatch"));
        bound.buffer = binding.buffer->share();
    } else {
        if (!*binding.image || !equal(std::get<ImageDesc>(resource.description), binding.image->description()))
            return std::unexpected(invalid("external image description mismatch"));
        bound.image = binding.image->share();
    }
    const auto desc = bound.image ? bound.image.description() : ImageDesc{};
    const auto count = buffer ? std::size_t{1} : static_cast<std::size_t>(desc.mip_levels) * desc.array_layers;
    if (!binding.expected_states.empty() && binding.expected_states.size() != count)
        return std::unexpected(invalid("external expected state count mismatch"));
    for (std::size_t i = 0; i < count; ++i) {
        auto state = buffer ? bound.buffer.state() : bound.image.state(static_cast<std::uint32_t>(i % desc.mip_levels),
            static_cast<std::uint32_t>(i / desc.mip_levels));
        if (!state) return std::unexpected(state.error());
        if (!binding.expected_states.empty() && *state != binding.expected_states[i])
            return std::unexpected(Error{ErrorCode::conflict, "external expected state is stale"});
        if (resource.initialized && !state->initialized)
            return std::unexpected(Error{ErrorCode::invalid_state, "external resource lacks declared initialized contents"});
    }
    return {};
}
Result<void> prepare_ray_resource(detail::ExecutionState& result,CommandBatch& batch,std::size_t index,
    const AccessDescription& access,std::optional<std::size_t> pass,bool capture_sync) {
    auto& bound=result.resources[index];
    auto before=bound.acceleration ? batch.state(bound.acceleration) : batch.state(bound.shader_table);
    if (!before) return std::unexpected(before.error());
    if (capture_sync) { auto target=access.state; target.initialized=before->initialized; result.synchronization.push_back({pass,index,0,0,*before,target}); }
    if (bound.shader_table) return batch.prepare_trace(bound.shader_table,{});
    if (access.full_overwrite) return batch.prepare_build(bound.acceleration);
    return batch.prepare_acceleration_read(bound.acceleration,access.state.stages==vk::PipelineStageFlagBits2::eRayTracingShaderKHR);
}
Result<void> capture(detail::ExecutionState& result, CommandBatch& batch, std::span<const ResourceUse> uses,
    std::span<const std::size_t> indices, std::optional<std::size_t> pass) {
    for (std::size_t i = 0; i < uses.size(); ++i) {
        const auto& use = uses[i];
        const auto append = [&](std::uint32_t mip, std::uint32_t layer) -> Result<void> {
            auto before = use.buffer ? batch.state(*use.buffer) : batch.state(*use.image,mip,layer);
            if (!before) return std::unexpected(before.error());
            auto target = use.state; target.initialized = before->initialized;
            result.synchronization.push_back({pass,indices[i],mip,layer,*before,target});
            return {};
        };
        if (use.buffer) { if (auto status = append(0,0); !status) return status; }
        else for (std::uint32_t layer = use.range.baseArrayLayer; layer < use.range.baseArrayLayer + use.range.layerCount; ++layer)
            for (std::uint32_t mip = use.range.baseMipLevel; mip < use.range.baseMipLevel + use.range.levelCount; ++mip)
                if (auto status = append(mip,layer); !status) return status;
    }
    return {};
}
Error pass_error(const PlannedPass& pass, std::size_t index, const Error& error) {
    return error.with_context("graph pass #" + std::to_string(index) + " '" + std::string(pass.name) + "'");
}
GpuZoneKind zone_kind(const PlannedPass& pass) {
    vk::PipelineStageFlags2 stages{};
    for (const auto& use : pass.uses) stages |= use.access.state.stages;
    if (stages & vk::PipelineStageFlagBits2::eComputeShader) return GpuZoneKind::compute;
    if (stages & (vk::PipelineStageFlagBits2::eAllGraphics | vk::PipelineStageFlagBits2::eVertexShader |
        vk::PipelineStageFlagBits2::eFragmentShader | vk::PipelineStageFlagBits2::eColorAttachmentOutput |
        vk::PipelineStageFlagBits2::eEarlyFragmentTests | vk::PipelineStageFlagBits2::eLateFragmentTests)) return GpuZoneKind::draw;
    if (stages & (vk::PipelineStageFlagBits2::eAllTransfer | vk::PipelineStageFlagBits2::eCopy |
        vk::PipelineStageFlagBits2::eClear | vk::PipelineStageFlagBits2::eBlit)) return GpuZoneKind::transfer;
    return GpuZoneKind::other;
}
}

const PlannedPass& PassContext::description() const noexcept { return plan_.passes()[pass_]; }
Result<const Buffer*> PassContext::buffer(std::size_t resource) const {
    for (const auto& use : description().uses)
        if (use.resource == resource && state_.resources[resource].buffer) return &state_.resources[resource].buffer;
    return std::unexpected(missing());
}
Result<const Image*> PassContext::image(std::size_t resource) const {
    for (const auto& use : description().uses)
        if (use.resource == resource && state_.resources[resource].image) return &state_.resources[resource].image;
    return std::unexpected(missing());
}
bool PassContext::buffer_range(std::size_t resource, vk::DeviceSize offset, vk::DeviceSize size) const {
    for (const auto& use : description().uses) if (use.resource == resource) {
        const auto& access = use.access;
        return offset >= access.offset && offset - access.offset <= access.size && size <= access.size - (offset - access.offset);
    }
    return false;
}
bool PassContext::image_range(std::size_t resource, const vk::ImageSubresourceRange& range) const {
    if (!range.levelCount || !range.layerCount) return false;
    for (const auto& use : description().uses) if (use.resource == resource) {
        const auto& a = use.access.range;
        if (range.aspectMask == a.aspectMask && range.baseMipLevel >= a.baseMipLevel &&
            range.baseMipLevel - a.baseMipLevel <= a.levelCount && range.levelCount <= a.levelCount - (range.baseMipLevel - a.baseMipLevel) &&
            range.baseArrayLayer >= a.baseArrayLayer && range.baseArrayLayer - a.baseArrayLayer <= a.layerCount &&
            range.layerCount <= a.layerCount - (range.baseArrayLayer - a.baseArrayLayer)) return true;
    }
    return false;
}
Result<void> PassContext::copy_buffer(std::size_t source, std::size_t destination, vk::DeviceSize size,
    vk::DeviceSize source_offset, vk::DeviceSize destination_offset) {
    auto src = buffer(source); if (!src) return std::unexpected(src.error());
    auto dst = buffer(destination); if (!dst) return std::unexpected(dst.error());
    if (!buffer_range(source, source_offset, size) || !buffer_range(destination, destination_offset, size)) return std::unexpected(range_error());
    return batch_.copy_buffer(**src, **dst, size, source_offset, destination_offset);
}
Result<void> PassContext::copy_to_image(std::size_t source, std::size_t destination, const ImageCopyRegion& region) {
    auto src = buffer(source); if (!src) return std::unexpected(src.error());
    auto dst = image(destination); if (!dst) return std::unexpected(dst.error());
    auto bytes = image_copy_bytes((*dst)->description(), region); if (!bytes) return std::unexpected(bytes.error());
    if (!buffer_range(source, region.buffer_offset, *bytes) ||
        !image_range(destination, {vk::ImageAspectFlagBits::eColor, region.mip, 1, region.layer, 1})) return std::unexpected(range_error());
    return batch_.copy_to_image(**src, **dst, region);
}
Result<void> PassContext::copy_to_buffer(std::size_t source, std::size_t destination, const ImageCopyRegion& region) {
    auto src = image(source); if (!src) return std::unexpected(src.error());
    auto dst = buffer(destination); if (!dst) return std::unexpected(dst.error());
    auto bytes = image_copy_bytes((*src)->description(), region); if (!bytes) return std::unexpected(bytes.error());
    if (!buffer_range(destination, region.buffer_offset, *bytes) ||
        !image_range(source, {vk::ImageAspectFlagBits::eColor, region.mip, 1, region.layer, 1})) return std::unexpected(range_error());
    return batch_.copy_to_buffer(**src, **dst, region);
}
Result<void> PassContext::fill(std::size_t resource, std::uint32_t value) {
    auto target = buffer(resource); if (!target) return std::unexpected(target.error());
    if (!buffer_range(resource, 0, (*target)->size())) return std::unexpected(range_error());
    return batch_.fill(**target, value);
}
Result<void> PassContext::clear(std::size_t resource, const vk::ClearColorValue& color, const vk::ImageSubresourceRange& range) {
    auto target = image(resource); if (!target) return std::unexpected(target.error());
    if (!image_range(resource, range)) return std::unexpected(range_error());
    return batch_.clear(**target, color, range);
}
Result<void> PassContext::clear_depth(std::size_t resource, float depth, const vk::ImageSubresourceRange& range) {
    auto target = image(resource); if (!target) return std::unexpected(target.error());
    if (!image_range(resource, range)) return std::unexpected(range_error());
    return batch_.clear_depth(**target, depth, range);
}
Result<ComputeEncoder> PassContext::compute() { return batch_.compute(); }
Result<const AccelerationStructure*> PassContext::acceleration(std::size_t resource) const {
    for (const auto& use : description().uses)
        if (use.resource==resource && state_.resources[resource].acceleration) return &state_.resources[resource].acceleration;
    return std::unexpected(missing());
}
Result<void> PassContext::build(std::size_t resource) {
    auto target=acceleration(resource); if (!target) return std::unexpected(target.error());
    for (const auto& use : description().uses) if (use.resource==resource && use.access.full_overwrite) return batch_.build_prepared(**target);
    return std::unexpected(invalid("AS BUILD requires a declared build-write"));
}
Result<void> PassContext::trace(const RayTracingPipeline& pipeline,std::size_t table,std::span<const BindingSet* const> sets,
    std::span<const std::byte> constants,std::array<std::uint32_t,3> dimensions) {
    for (const auto& use : description().uses) if (use.resource==table && state_.resources[table].shader_table) {
        auto encoder=batch_.ray_tracing(); if (!encoder) return std::unexpected(encoder.error());
        return encoder->trace(pipeline,state_.resources[table].shader_table,sets,constants,dimensions);
    }
    return std::unexpected(missing());
}
Result<RenderEncoder> PassContext::begin_rendering(const RenderingDesc& description) { return batch_.begin_rendering(description); }

std::span<const Synchronization> Execution::synchronization() const noexcept { return state_ ? std::span<const Synchronization>{state_->synchronization} : std::span<const Synchronization>{}; }
Submission Execution::submission() const noexcept { return state_ ? state_->submission : Submission{}; }
std::span<const ExportedState> Execution::states() const noexcept { return state_ ? std::span<const ExportedState>{state_->states} : std::span<const ExportedState>{}; }
Result<const Buffer*> Execution::buffer(std::size_t resource) const {
    if (state_ && resource < state_->resources.size() && state_->resources[resource].buffer) return &state_->resources[resource].buffer;
    return std::unexpected(invalid("execution resource is not a buffer output"));
}
Result<const Image*> Execution::image(std::size_t resource) const {
    if (state_ && resource < state_->resources.size() && state_->resources[resource].image) return &state_->resources[resource].image;
    return std::unexpected(invalid("execution resource is not an image output"));
}
Result<const AccelerationStructure*> Execution::acceleration(std::size_t resource) const {
    if (state_ && resource<state_->resources.size() && state_->resources[resource].acceleration) return &state_->resources[resource].acceleration;
    return std::unexpected(invalid("execution resource is not an AS output"));
}
Result<const ShaderBindingTable*> Execution::shader_table(std::size_t resource) const {
    if (state_ && resource<state_->resources.size() && state_->resources[resource].shader_table) return &state_->resources[resource].shader_table;
    return std::unexpected(invalid("execution resource is not an SBT output"));
}

Result<Execution> execute(const CompiledGraph& plan, SubmissionQueue& queue, const ExecutionDesc& description) {
    DK_PROFILE_ZONE("graph.execute");
    if (!plan) return std::unexpected(Error{ErrorCode::invalid_state, "empty compiled graph"});
    const auto heap = detail::ExecutionAccess::state(plan)->resource;
    if (heap.state() != memory::ResourceState::open) return std::unexpected(Error{ErrorCode::invalid_state, "graph Memory resource is closed"});
    try {
        const auto resources = plan.resources();
        auto result = memory::make_shared_in<detail::ExecutionState>(heap, heap, resources.size());
        Vector<const PassCallback*> callbacks(plan.passes().size(), nullptr, memory::Allocator<const PassCallback*>{heap});
        Vector<FinalAccess> final(0, memory::Allocator<FinalAccess>{heap});
        Vector<ResourceUse> uses(0, memory::Allocator<ResourceUse>{heap});
        Vector<std::size_t> indices(0, memory::Allocator<std::size_t>{heap});
        const auto resource_error = [&](std::size_t i, const Error& error) {
            return error.with_context("graph resource #" + std::to_string(i) + " '" + std::string(resources[i].name) + "'");
        };
        for (const auto& callback : description.callbacks) {
            if (callback.pass >= callbacks.size() || !callback.record || callbacks[callback.pass])
                return std::unexpected(invalid("invalid or duplicate pass callback"));
            callbacks[callback.pass] = &callback;
        }
        for (const auto pass : plan.order()) if (!callbacks[pass]) return std::unexpected(invalid("missing retained pass callback"));
        for (const auto& binding : description.bindings) {
            if (binding.resource >= resources.size() || resources[binding.resource].lifetime != Lifetime::external || !resources[binding.resource].retained)
                return std::unexpected(invalid("binding requires a retained external resource"));
            auto& bound = result->resources[binding.resource];
            if (bound) return std::unexpected(invalid("duplicate external binding"));
            if (auto imported = import_resource(bound, resources[binding.resource], binding); !imported) return std::unexpected(resource_error(binding.resource, imported.error()));
            for (std::size_t i = 0; i < resources.size(); ++i) if (i != binding.resource) {
                const auto& other = result->resources[i];
                if ((bound.buffer && other.buffer && bound.buffer.handle() == other.buffer.handle()) ||
                    (bound.image && other.image && bound.image.handle() == other.image.handle()) ||
                    (bound.acceleration && other.acceleration && bound.acceleration.handle()==other.acceleration.handle()) ||
                    (bound.shader_table && other.shader_table && bound.shader_table.regions().raygeneration.deviceAddress==other.shader_table.regions().raygeneration.deviceAddress))
                    return std::unexpected(invalid("one physical resource cannot bind multiple graph resources"));
            }
        }
        for (std::size_t i = 0; i < resources.size(); ++i) {
            const auto& resource = resources[i];
            if (resource.retained && resource.lifetime == Lifetime::external && !result->resources[i])
                return std::unexpected(invalid("missing retained external binding"));
        }
        for (std::size_t i=0; i<resources.size(); ++i) if (result->resources[i].acceleration) {
            Vector<const Buffer*> inputs(0,memory::Allocator<const Buffer*>{heap});
            Vector<const AccelerationStructure*> children(0,memory::Allocator<const AccelerationStructure*>{heap});
            for (const auto dependency : resources[i].dependencies) {
                const auto& bound=result->resources[dependency];
                if (bound.buffer) inputs.push_back(&bound.buffer); else children.push_back(&bound.acceleration);
            }
            if (auto valid=result->resources[i].acceleration.validate_dependencies(inputs,children); !valid) return std::unexpected(resource_error(i,valid.error()));
        }
        for (const auto& request : description.final_accesses) {
            if (request.resource >= resources.size()) return std::unexpected(invalid("final access resource is out of range"));
            const auto& resource = resources[request.resource];
            if (!resource.retained || (!resource.output && resource.lifetime != Lifetime::external) || request.access.full_overwrite || request.access.state.initialized)
                return std::unexpected(invalid("final access requires retained external/output and cannot initialize contents"));
            const auto* buffer = std::get_if<BufferDesc>(&resource.description);
            const bool ray=std::holds_alternative<AccelerationDesc>(resource.description) || std::holds_alternative<ShaderTableDesc>(resource.description);
            if (ray) {
                const bool as=std::holds_alternative<AccelerationDesc>(resource.description);
                const auto& access=request.access;
                if ((access.state.stages!=vk::PipelineStageFlagBits2::eRayTracingShaderKHR && !(as && access.state.stages==vk::PipelineStageFlagBits2::eAccelerationStructureBuildKHR)) ||
                    access.state.access!=(as ? vk::AccessFlags2{vk::AccessFlagBits2::eAccelerationStructureReadKHR} : vk::AccessFlags2{vk::AccessFlagBits2::eShaderRead}) ||
                    access.state.layout!=vk::ImageLayout::eUndefined || access.offset || (access.size!=1 && access.size!=VK_WHOLE_SIZE) || access.range!=vk::ImageSubresourceRange{})
                    return std::unexpected(invalid("invalid AS/SBT final read access"));
                for (const auto& prior : final) if (prior.resource==request.resource) return std::unexpected(invalid("duplicate AS/SBT final access"));
                final.push_back(request); continue;
            }
            auto access = buffer ? validate_buffer_access(*buffer, request.access) : validate_image_access(std::get<ImageDesc>(resource.description), request.access);
            if (!access) return std::unexpected(access.error());
            // Reject overlap before recording any callbacks; prepare enforces the same rule.
            for (const auto& previous : final) if (previous.resource == request.resource) {
                const auto& a = previous.access.range; const auto& b = access->range;
                if (buffer || (a.baseMipLevel < b.baseMipLevel + b.levelCount && b.baseMipLevel < a.baseMipLevel + a.levelCount &&
                    a.baseArrayLayer < b.baseArrayLayer + b.layerCount && b.baseArrayLayer < a.baseArrayLayer + a.layerCount))
                    return std::unexpected(invalid("overlapping final accesses"));
            }
            final.push_back({request.resource, *access});
        }
        auto batch_result = queue.begin(); if (!batch_result) return std::unexpected(batch_result.error());
        auto batch = std::move(*batch_result);
        // Retain all imports before callbacks: enforces device, WSI and recording reservations.
        for (std::size_t i = 0; i < resources.size(); ++i) {
            auto& bound = result->resources[i];
            if (!bound) continue;
            auto retained = bound.buffer ? batch.retain(bound.buffer) : bound.image ? batch.retain(bound.image) :
                bound.acceleration ? batch.retain(bound.acceleration) : batch.retain(bound.shader_table);
            if (!retained) return std::unexpected(resource_error(i, retained.error()));
        }
        std::size_t next_allocation = 0;
        for (std::size_t position = 0; position < plan.order().size(); ++position) {
            while (next_allocation < plan.allocations().size() && plan.allocations()[next_allocation].create_before == position) {
                const auto index = plan.allocations()[next_allocation++].resource;
                auto& bound = result->resources[index];
                if (const auto* desc = std::get_if<BufferDesc>(&resources[index].description)) {
                    auto created = queue.create_buffer(*desc); if (!created) return std::unexpected(resource_error(index, created.error()));
                    bound.buffer = std::move(*created);
                } else {
                    auto created = queue.create_image(std::get<ImageDesc>(resources[index].description)); if (!created) return std::unexpected(resource_error(index, created.error()));
                    bound.image = std::move(*created);
                }
            }
            const auto index = plan.order()[position];
            const auto& pass = plan.passes()[index];
            if (auto started = batch.begin_gpu_zone(std::string_view{pass.name}.substr(0,1024), zone_kind(pass)); !started)
                return std::unexpected(pass_error(pass,index,started.error()));
            uses.clear(); indices.clear();
            for (const auto& use : pass.uses) {
                if (result->resources[use.resource].acceleration || result->resources[use.resource].shader_table) continue;
                uses.push_back(use_for(result->resources[use.resource], use.access));
                if (description.capture_synchronization) indices.push_back(use.resource);
            }
            if (description.capture_synchronization)
                if (auto status = capture(*result,batch,uses,indices,index); !status) return std::unexpected(pass_error(pass,index,status.error()));
            if (auto prepared = batch.prepare(uses); !prepared) return std::unexpected(pass_error(pass,index,prepared.error()));
            // All reads before BUILD preparation; CPU validation requires its complete declared input closure.
            for (const bool writes : {false,true}) for (const auto& use : pass.uses) {
                const auto& bound=result->resources[use.resource];
                if ((!bound.acceleration && !bound.shader_table) || use.access.full_overwrite!=writes) continue;
                if (auto prepared=prepare_ray_resource(*result,batch,use.resource,use.access,index,description.capture_synchronization); !prepared)
                    return std::unexpected(pass_error(pass,index,prepared.error()));
            }
            auto context = detail::ExecutionAccess::context(plan, *result, batch, index);
            auto recorded = [&]() -> Result<void> {
                try { return callbacks[index]->record(context, callbacks[index]->user_data); }
                catch (const std::exception& error) { return std::unexpected(Error{ErrorCode::internal_error,error.what()}); }
                catch (...) { return std::unexpected(Error{ErrorCode::internal_error,"unknown callback exception"}); }
            }();
            if (!recorded) return std::unexpected(pass_error(pass,index,recorded.error()));
            if (auto finished = batch.finish_pass(); !finished) return std::unexpected(pass_error(pass,index,finished.error()));
            if (auto ended = batch.end_gpu_zone(); !ended) return std::unexpected(pass_error(pass,index,ended.error()));
            for (const auto& allocation : plan.allocations()) if (allocation.release_after == position)
                result->resources[allocation.resource] = {};
        }
        uses.clear(); indices.clear();
        for (const auto& request : final) {
            const auto& bound=result->resources[request.resource];
            if (bound.acceleration || bound.shader_table) {
                if (auto prepared=prepare_ray_resource(*result,batch,request.resource,request.access,{},description.capture_synchronization); !prepared) return std::unexpected(prepared.error());
                continue;
            }
            uses.push_back(use_for(result->resources[request.resource], request.access));
            if (description.capture_synchronization) indices.push_back(request.resource);
        }
        if (description.capture_synchronization)
            if (auto status = capture(*result,batch,uses,indices,{}); !status) return std::unexpected(status.error().with_context("graph final access"));
        if (auto prepared = batch.prepare(uses); !prepared) return std::unexpected(prepared.error());
        // Snapshot before submit. No allocation or fallible work follows successful submission.
        for (std::size_t i = 0; i < resources.size(); ++i) {
            const auto& resource = resources[i];
            if (!resource.retained || (!resource.output && resource.lifetime != Lifetime::external)) continue;
            const auto& bound = result->resources[i];
            if (bound.buffer) {
                auto state = batch.state(bound.buffer); if (!state) return std::unexpected(state.error());
                result->states.push_back({i, 0, 0, *state});
            } else if (bound.image) {
                const auto desc = bound.image.description();
                for (std::uint32_t layer = 0; layer < desc.array_layers; ++layer) for (std::uint32_t mip = 0; mip < desc.mip_levels; ++mip) {
                    auto state = batch.state(bound.image, mip, layer); if (!state) return std::unexpected(state.error());
                    result->states.push_back({i, mip, layer, *state});
                }
            } else {
                auto state=bound.acceleration ? batch.state(bound.acceleration) : batch.state(bound.shader_table);
                if (!state) return std::unexpected(state.error());
                std::uint64_t generation=0;
                if (bound.acceleration) { auto live=bound.acceleration.state(); if (!live) return std::unexpected(live.error()); generation=live->generation; }
                result->states.push_back({i,0,0,*state,generation});
            }
        }
        auto submission = queue.submit(std::move(batch)); if (!submission) return std::unexpected(submission.error().with_context("graph submit"));
        DK_PROFILE_ZONE_VALUE(submission->value());
        result->submission = *submission;
        for (auto& exported : result->states) if (result->resources[exported.resource].acceleration && exported.state.initialized && !exported.generation)
            exported.generation=submission->value();
        for (std::size_t i = 0; i < resources.size(); ++i) if (!resources[i].output) result->resources[i] = {};
        return detail::ExecutionAccess::publish(std::move(result));
    } catch (const std::exception& error) {
        return std::unexpected(Error{ErrorCode::internal_error, "graph execution failed: " + std::string(error.what())});
    } catch (...) {
        return std::unexpected(Error{ErrorCode::internal_error, "graph execution callback threw an unknown exception"});
    }
}
} // namespace dk::graphics::graph

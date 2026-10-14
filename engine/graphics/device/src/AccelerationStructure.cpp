#include "AccelerationInternal.hpp"
#include "AccelerationPolicy.hpp"
#include "CommandInternal.hpp"
#include <cstring>

namespace dk::graphics::detail {
void release_acceleration_uses(Vector<AccelerationUse>& uses, bool recording, bool completed) noexcept
{
    for (auto& use : uses) {
        if (recording) use.resource->reserved = false;
        if (completed && use.built) {
            use.resource->state.status = AccelerationStructureStatus::completed;
            use.resource->inputs.clear();
            use.resource->geometries.clear();
            use.resource->ranges.clear();
        }
    }
    uses.clear();
}
void commit_acceleration_uses(Vector<AccelerationUse>& uses, std::uint64_t submission) noexcept
{
    for (auto& use : uses) {
        use.resource->reserved = false;
        use.resource->state.access = use.access;
        if (use.built) {
            use.resource->state.status = AccelerationStructureStatus::submitted;
            use.resource->state.generation = submission;
        }
    }
}
namespace {
Result<void> rt_enabled(const QueueState& queue)
{
    if (!queue.owner->device.ray_tracing_enabled())
        return std::unexpected(Error{ErrorCode::not_supported, "acceleration structures require enabled ray tracing"});
    return {};
}
Result<void> input_owner(const QueueState& queue, const Buffer* buffer)
{
    if (!buffer || !*buffer || ObjectAccess::state(*buffer)->owner != queue.owner)
        return std::unexpected(Error{ErrorCode::invalid_argument, "AS input is empty or belongs to another submission domain"});
    return {};
}
Result<void> allocate_acceleration(QueueState& queue, const ResourceFactory& factory, AccelerationState& state)
{
    Vector<std::uint32_t> counts{memory::Allocator<std::uint32_t>{queue.resource}};
    for (const auto& range : state.ranges) counts.push_back(range.primitiveCount);
    const auto& device = queue.owner->device;
    const auto sizes = device.logical_device().getAccelerationStructureBuildSizesKHR(vk::AccelerationStructureBuildTypeKHR::eDevice,
        state.build_info(), counts);
    state.sizes = {sizes.accelerationStructureSize, sizes.buildScratchSize,
        device.adapter().ray_tracing.acceleration_structure_limits.minAccelerationStructureScratchOffsetAlignment};
    if (!state.sizes.storage) return std::unexpected(Error{ErrorCode::internal_error, "driver returned zero AS storage size"});
    if (auto capacity = scratch_capacity(state.sizes.scratch, state.sizes.scratch_alignment); !capacity) return std::unexpected(capacity.error());
    auto storage = factory.create_buffer({state.sizes.storage, vk::BufferUsageFlagBits::eAccelerationStructureStorageKHR | vk::BufferUsageFlagBits::eShaderDeviceAddress});
    if (!storage) return std::unexpected(storage.error());
    state.storage = std::move(*storage);
    vk::AccelerationStructureCreateInfoKHR create{};
    create.setBuffer(state.storage.handle()).setSize(state.sizes.storage).setType(state.build_info().type);
    try {
        state.acceleration = vk::raii::AccelerationStructureKHR{device.logical_device(), create};
    } catch (const vk::SystemError& error) {
        return std::unexpected(queue.failure("create acceleration structure", static_cast<VkResult>(error.code().value())));
    }
    if (auto status = object_creation_status(queue, "create acceleration structure"); !status) return status;
    state.address = device.logical_device().getAccelerationStructureAddressKHR(vk::AccelerationStructureDeviceAddressInfoKHR{*state.acceleration});
    if (!state.address) return std::unexpected(Error{ErrorCode::internal_error, "driver returned zero AS address"});
    return {};
}
AccelerationUse* find_acceleration(BatchState& batch, const std::shared_ptr<AccelerationState>& resource)
{
    for (auto& use : batch.accelerations) if (use.resource == resource) return &use;
    return nullptr;
}
Result<void> check_acceleration(BatchState& batch, const std::shared_ptr<AccelerationState>& resource)
{
    if (!resource || resource->owner != batch.queue->owner)
        return std::unexpected(Error{ErrorCode::invalid_argument, "AS is empty or belongs to another submission domain"});
    if (resource->reserved && !find_acceleration(batch, resource))
        return std::unexpected(Error{ErrorCode::conflict, "AS is reserved by another recording batch"});
    return {};
}
struct AccelerationRollback {
    BatchState& batch;
    std::size_t count;
    bool committed = false;
    ~AccelerationRollback() {
        if (!committed) while (batch.accelerations.size() > count) {
            batch.accelerations.back().resource->reserved = false;
            batch.accelerations.pop_back();
        }
    }
};
} // namespace
} // namespace dk::graphics::detail

namespace dk::graphics {
vk::AccelerationStructureKHR AccelerationStructure::handle() const noexcept { return state_ ? *state_->acceleration : vk::AccelerationStructureKHR{}; }
AccelerationStructureKind AccelerationStructure::kind() const noexcept { return state_ ? state_->kind : AccelerationStructureKind::bottom_level; }
AccelerationStructureSizes AccelerationStructure::sizes() const noexcept { return state_ ? state_->sizes : AccelerationStructureSizes{}; }
Result<vk::DeviceAddress> AccelerationStructure::device_address() const
{
    if (!state_ || state_->owner->lost) return std::unexpected(Error{ErrorCode::invalid_state, "AS is empty or its device is lost"});
    return state_->address;
}
Result<AccelerationStructureState> AccelerationStructure::state() const
{
    if (!state_) return std::unexpected(Error{ErrorCode::invalid_state, "empty AS"});
    auto result = state_->state;
    if (state_->owner->lost) { result.status = AccelerationStructureStatus::device_lost; result.access.initialized = false; }
    return result;
}
Result<AccelerationStructure> ResourceFactory::create_blas(std::span<const TriangleGeometry> geometries) const
{
    auto checked = detail::factory_queue(queue_);
    if (!checked) return std::unexpected(checked.error());
    auto& queue = **checked;
    if (auto enabled = detail::rt_enabled(queue); !enabled) return std::unexpected(enabled.error());
    const auto& limits = queue.owner->device.adapter().ray_tracing.acceleration_structure_limits;
    if (auto count = detail::acceleration_count(geometries.size(), limits.maxGeometryCount); !count) return std::unexpected(count.error());
    std::uint64_t total = 0;
    for (const auto& geometry : geometries) {
        if (auto valid = detail::input_owner(queue, geometry.vertices); !valid) return std::unexpected(valid.error());
        if (geometry.indices) if (auto valid = detail::input_owner(queue, geometry.indices); !valid) return std::unexpected(valid.error());
        const auto index_desc = geometry.indices ? geometry.indices->description() : BufferDesc{};
        if (auto valid = detail::triangle_description(geometry, geometry.vertices->description(), geometry.indices ? &index_desc : nullptr,
            limits.maxPrimitiveCount); !valid) return std::unexpected(valid.error());
        if (geometry.primitive_count > limits.maxPrimitiveCount-total)
            return std::unexpected(Error{ErrorCode::invalid_argument, "BLAS total primitives exceed device limit"});
        total += geometry.primitive_count;
    }
    if (!(queue.owner->device.physical_device().getFormatProperties(vk::Format::eR32G32B32Sfloat).bufferFeatures &
        vk::FormatFeatureFlagBits::eAccelerationStructureVertexBufferKHR))
        return std::unexpected(Error{ErrorCode::not_supported, "device does not support AS float3 vertex format"});
    auto state = memory::make_shared_in<detail::AccelerationState>(queue.resource, queue.owner, queue.resource, AccelerationStructureKind::bottom_level);
    state->geometry_count = static_cast<std::uint32_t>(geometries.size());
    for (const auto& input : geometries) {
        auto base = input.vertices->device_address();
        if (!base) return std::unexpected(base.error());
        auto address = detail::acceleration_address(*base, input.vertex_offset, vk::DeviceSize{input.vertex_count-1}*input.vertex_stride+12, 4);
        if (!address) return std::unexpected(address.error());
        vk::AccelerationStructureGeometryTrianglesDataKHR triangles{};
        triangles.setVertexFormat(input.vertex_format).setVertexData(vk::DeviceOrHostAddressConstKHR{*address})
            .setVertexStride(input.vertex_stride).setMaxVertex(input.vertex_count-1).setIndexType(vk::IndexType::eNoneKHR);
        state->inputs.push_back(input.vertices->share());
        if (input.indices) {
            base = input.indices->device_address();
            if (!base) return std::unexpected(base.error());
            const auto width = input.index_type == vk::IndexType::eUint16 ? 2u : 4u;
            address = detail::acceleration_address(*base, input.index_offset, vk::DeviceSize{input.primitive_count}*3*width, width);
            if (!address) return std::unexpected(address.error());
            triangles.setIndexType(input.index_type).setIndexData(vk::DeviceOrHostAddressConstKHR{*address});
            state->inputs.push_back(input.indices->share());
        }
        vk::AccelerationStructureGeometryKHR geometry{};
        geometry.setGeometryType(vk::GeometryTypeKHR::eTriangles).setFlags(input.opaque ? vk::GeometryFlagBitsKHR::eOpaque : vk::GeometryFlagsKHR{});
        geometry.geometry.setTriangles(triangles);
        state->geometries.push_back(geometry);
        state->ranges.emplace_back(input.primitive_count, 0, 0, 0);
    }
    if (auto result = detail::allocate_acceleration(queue, *this, *state); !result) return std::unexpected(result.error());
    return AccelerationStructure{std::move(state)};
}
Result<AccelerationStructure> ResourceFactory::create_tlas(std::span<const AccelerationStructureInstance> instances) const
{
    auto checked = detail::factory_queue(queue_);
    if (!checked) return std::unexpected(checked.error());
    auto& queue = **checked;
    if (auto enabled = detail::rt_enabled(queue); !enabled) return std::unexpected(enabled.error());
    if (auto count = detail::acceleration_count(instances.size(), queue.owner->device.adapter().ray_tracing.acceleration_structure_limits.maxInstanceCount); !count)
        return std::unexpected(count.error());
    for (const auto& instance : instances) {
        if (auto valid = detail::instance_description(instance); !valid) return std::unexpected(valid.error());
        if (!instance.blas || !*instance.blas || instance.blas->kind() != AccelerationStructureKind::bottom_level ||
            instance.blas->state_->owner != queue.owner)
            return std::unexpected(Error{ErrorCode::invalid_argument, "TLAS instance requires an owned BLAS from its submission domain"});
    }
    auto state = memory::make_shared_in<detail::AccelerationState>(queue.resource, queue.owner, queue.resource, AccelerationStructureKind::top_level);
    Vector<VkAccelerationStructureInstanceKHR> native{memory::Allocator<VkAccelerationStructureInstanceKHR>{queue.resource}};
    for (const auto& instance : instances) {
        VkAccelerationStructureInstanceKHR value{};
        std::memcpy(value.transform.matrix, instance.transform.data(), sizeof(value.transform));
        value.instanceCustomIndex = instance.custom_index;
        value.instanceShaderBindingTableRecordOffset = instance.sbt_record_offset;
        value.mask = instance.mask;
        value.flags = static_cast<VkGeometryInstanceFlagsKHR>(instance.flags);
        value.accelerationStructureReference = instance.blas->state_->address;
        native.push_back(value);
        state->hit_ranges.push_back({instance.sbt_record_offset, instance.blas->state_->geometry_count});
        if (std::find(state->children.begin(), state->children.end(), instance.blas->state_) == state->children.end())
            state->children.push_back(instance.blas->state_);
    }
    auto input = create_buffer({native.size()*sizeof(VkAccelerationStructureInstanceKHR), detail::acceleration_input_usage, BufferMemory::upload});
    if (!input) return std::unexpected(input.error());
    if (auto written = input->write(0, std::as_bytes(std::span{native})); !written) return std::unexpected(written.error());
    auto base = input->device_address();
    if (!base) return std::unexpected(base.error());
    auto address = detail::acceleration_address(*base, 0, input->size(), 16);
    if (!address) return std::unexpected(address.error());
    vk::AccelerationStructureGeometryKHR geometry{};
    geometry.setGeometryType(vk::GeometryTypeKHR::eInstances);
    geometry.geometry.setInstances(vk::AccelerationStructureGeometryInstancesDataKHR{false, vk::DeviceOrHostAddressConstKHR{*address}});
    state->geometries.push_back(geometry);
    state->ranges.emplace_back(static_cast<std::uint32_t>(instances.size()), 0, 0, 0);
    state->inputs.push_back(std::move(*input));
    if (auto result = detail::allocate_acceleration(queue, *this, *state); !result) return std::unexpected(result.error());
    return AccelerationStructure{std::move(state)};
}
Result<void> CommandBatch::build(const AccelerationStructure& candidate)
{
    if (!state_) return std::unexpected(Error{ErrorCode::invalid_state, "empty batch"});
    auto& batch = *state_;
    if (auto valid = detail::outside_rendering(batch); !valid) return valid;
    const auto& target = candidate.state_;
    if (auto valid = detail::check_acceleration(batch, target); !valid) return valid;
    if (target->state.status != AccelerationStructureStatus::candidate || detail::find_acceleration(batch, target))
        return std::unexpected(Error{ErrorCode::invalid_state, "AS BUILD requires an unbuilt candidate"});
    for (const auto& child : target->children) {
        if (auto valid = detail::check_acceleration(batch, child); !valid) return valid;
        const auto* use = detail::find_acceleration(batch, child);
        if (!(use ? use->access.initialized : child->state.access.initialized))
            return std::unexpected(Error{ErrorCode::invalid_state, "TLAS requires previously built/submitted BLAS content"});
    }
    constexpr auto build_stage = vk::PipelineStageFlagBits2::eAccelerationStructureBuildKHR;
    Vector<detail::ResolvedUse> inputs{memory::Allocator<detail::ResolvedUse>{batch.queue->resource}};
    for (const auto& input : target->inputs) {
        const auto& resource = detail::ObjectAccess::state(input);
        if (auto valid = batch.check(resource); !valid) return valid;
        const auto* local = batch.find(resource);
        if (!(local ? local->states[0].initialized : resource->states[0].initialized))
            return std::unexpected(Error{ErrorCode::invalid_state, "AS input buffer requires fully initialized contents"});
        if (std::none_of(inputs.begin(), inputs.end(), [&](const auto& use) { return use.resource == resource; }))
            inputs.push_back({resource, {build_stage, vk::AccessFlagBits2::eShaderRead}, 0, input.size(), {}, false});
    }
    Buffer scratch;
    vk::DeviceAddress scratch_data = 0;
    if (target->sizes.scratch) {
        auto capacity = detail::scratch_capacity(target->sizes.scratch, target->sizes.scratch_alignment);
        if (!capacity) return std::unexpected(capacity.error());
        auto buffer = detail::ObjectAccess::factory(batch).create_buffer({*capacity, vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eShaderDeviceAddress});
        if (!buffer) return std::unexpected(buffer.error());
        scratch = std::move(*buffer);
        auto base = scratch.device_address();
        if (!base) return std::unexpected(base.error());
        auto address = detail::scratch_address(*base, scratch.size(), target->sizes.scratch, target->sizes.scratch_alignment);
        if (!address) return std::unexpected(address.error());
        scratch_data = *address;
    }
    detail::RetainRollback buffers{batch};
    detail::AccelerationRollback accelerations{batch, batch.accelerations.size()};
    batch.accelerations.reserve(batch.accelerations.size()+target->children.size()+1);
    const auto retain_as = [&](const std::shared_ptr<detail::AccelerationState>& resource) {
        if (!detail::find_acceleration(batch, resource)) {
            batch.accelerations.push_back({resource, resource->state.access, false});
            resource->reserved = true;
        }
    };
    retain_as(target);
    for (const auto& child : target->children) retain_as(child);
    if (scratch) if (auto result = batch.retain(detail::ObjectAccess::state(scratch)); !result) return result;
    // All validation/allocation/retention precedes emission; no fallible work follows preparation.
    if (auto result = detail::prepare_resolved(batch, inputs); !result) return result;
    vk::MemoryBarrier2 dependency{};
    for (const auto& child : target->children) {
        const auto& prior = detail::find_acceleration(batch, child)->access;
        dependency.srcStageMask |= prior.stages;
        dependency.srcAccessMask |= prior.access;
    }
    dependency.setDstStageMask(build_stage).setDstAccessMask(vk::AccessFlagBits2::eAccelerationStructureReadKHR | vk::AccessFlagBits2::eAccelerationStructureWriteKHR);
    vk::DependencyInfo barriers{};
    barriers.setMemoryBarriers(dependency);
    batch.command().pipelineBarrier2(barriers);
    auto info = target->build_info();
    info.setDstAccelerationStructure(*target->acceleration).setScratchData(vk::DeviceOrHostAddressKHR{scratch_data});
    const auto* ranges = reinterpret_cast<const VkAccelerationStructureBuildRangeInfoKHR*>(target->ranges.data());
    batch.command().getDispatcher()->vkCmdBuildAccelerationStructuresKHR(static_cast<VkCommandBuffer>(*batch.command()), 1,
        reinterpret_cast<const VkAccelerationStructureBuildGeometryInfoKHR*>(&info), &ranges);
    for (const auto& child : target->children)
        detail::find_acceleration(batch, child)->access = {build_stage, vk::AccessFlagBits2::eAccelerationStructureReadKHR, vk::ImageLayout::eUndefined, true};
    auto& built = *detail::find_acceleration(batch, target);
    built.access = {build_stage, vk::AccessFlagBits2::eAccelerationStructureWriteKHR, vk::ImageLayout::eUndefined, true};
    built.built = true;
    buffers.committed = accelerations.committed = true;
    return {};
}
} // namespace dk::graphics

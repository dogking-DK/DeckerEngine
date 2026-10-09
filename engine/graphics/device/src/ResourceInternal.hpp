#pragma once
#include "SubmissionInternal.hpp"
#include "GpuProfilingInternal.hpp"
#include <dk/graphics/Transfer.hpp>
#include <algorithm>

namespace dk::graphics::detail {
struct PoolPage;
struct ObjectState;
struct EncoderState;
struct DeviceLifetime {
    explicit DeviceLifetime(Device&& value) : device(std::move(value)) {}
    Device device;
    bool lost = false;
};
struct ResourceState {
    ResourceState(std::shared_ptr<DeviceLifetime> value, memory::ResourceHandle resource)
        : owner(std::move(value)), states(memory::Allocator<AccessState>{resource}) {}
    std::shared_ptr<DeviceLifetime> owner;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    std::shared_ptr<void> external_owner;
    std::weak_ptr<BatchState> external_batch;
    BufferDesc buffer_desc;
    ImageDesc image_desc;
    Vector<AccessState> states;
    std::size_t uses = 0;
    bool reserved = false;
    ~ResourceState()
    {
        if (buffer) vmaDestroyBuffer(owner->device.allocator(), buffer, allocation);
        if (image && !external_owner) vmaDestroyImage(owner->device.allocator(), image, allocation);
    }
};
struct Use {
    Use(std::shared_ptr<ResourceState> value, memory::ResourceHandle memory)
        : resource(std::move(value)), states(resource->states.begin(), resource->states.end(), memory::Allocator<AccessState>{memory}),
          prepared(states.size(), 0, memory::Allocator<std::uint8_t>{memory}) {}
    std::shared_ptr<ResourceState> resource;
    Vector<AccessState> states;
    Vector<std::uint8_t> prepared;
};
enum class SlotPhase { free, recording, pending };
struct ReadbackState {
    ReadbackState(Buffer&& buffer, ReadbackDescription value) : staging(std::move(buffer)), description(value) {}
    Buffer staging;
    ReadbackDescription description;
    ReadbackStatus status = ReadbackStatus::unsubmitted;
};
inline void complete_requests(Vector<std::shared_ptr<ReadbackState>>& requests, ReadbackStatus status) noexcept
{ for (auto& request : requests) request->status = status; requests.clear(); }
struct Slot {
    explicit Slot(memory::ResourceHandle resource) : uses(memory::Allocator<Use>{resource}), objects(memory::Allocator<std::shared_ptr<ObjectState>>{resource}),
        requests(memory::Allocator<std::shared_ptr<ReadbackState>>{resource}) {}
    // Command buffer must be destroyed before its pool.
    vk::raii::CommandPool pool{nullptr};
    vk::raii::CommandBuffer command{nullptr};
    Vector<Use> uses;
    Vector<std::shared_ptr<ObjectState>> objects;
    Vector<std::shared_ptr<ReadbackState>> requests;
    SlotPhase phase = SlotPhase::free;
    std::shared_ptr<void> external_sync_owner;
    std::uint64_t value = 0;
    std::shared_ptr<GpuProfileState> profile;
};
inline void release_uses(Vector<Use>& uses, bool recording) noexcept
{
    for (auto& use : uses) {
        --use.resource->uses;
        if (recording) use.resource->reserved = false;
    }
    uses.clear();
}
struct QueueState {
    QueueState(memory::ResourceHandle resource_value, std::shared_ptr<DeviceLifetime> device_value)
        : resource(std::move(resource_value)), owner(std::move(device_value)), slots(memory::Allocator<Slot>{resource}),
          descriptor_pages(memory::Allocator<std::weak_ptr<PoolPage>>{resource}) {}
    memory::ResourceHandle resource;
    std::shared_ptr<DeviceLifetime> owner;
    vk::raii::Semaphore timeline{nullptr};
    Vector<Slot> slots;
    Vector<std::weak_ptr<PoolPage>> descriptor_pages;
    SubmissionApi api;
    std::shared_ptr<GpuProfiler> profiler;
    TracyGpuContext tracy;
    std::uint64_t submitted = 0, completed = 0;
    bool closed = false;
    VkResult object_creation_failure = VK_SUCCESS; // Private one-shot failure seam after native creation.
    Error failure(const char* operation, VkResult result)
    {
        if (result == VK_ERROR_DEVICE_LOST) owner->lost = true;
        return {result == VK_ERROR_FORMAT_NOT_SUPPORTED || result == VK_ERROR_FEATURE_NOT_PRESENT
                ? ErrorCode::not_supported : ErrorCode::internal_error,
            std::string(operation) + " failed: " + std::string(vulkan_result_name(result)) +
            " (" + std::to_string(static_cast<int>(result)) + ")"};
    }
    Result<void> accepting() const
    {
        if (owner->lost || closed)
            return std::unexpected(Error{ErrorCode::invalid_state, owner->lost ? "submission device is lost" : "submission queue is closed"});
        return {};
    }
    VkResult wait_value(std::uint64_t value, std::uint64_t timeout) const noexcept
    {
        const VkSemaphore semaphore = static_cast<VkSemaphore>(*timeline);
        const VkSemaphoreWaitInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, nullptr, 0, 1, &semaphore, &value};
        return api.wait(owner->device.native_device(), &info, timeout);
    }
    void collect(std::uint64_t value) noexcept
    {
        completed = std::max(completed, value);
        for (;;) {
            Slot* next = nullptr;
            for (auto& candidate : slots)
                if (candidate.phase == SlotPhase::pending && candidate.value <= completed && (!next || candidate.value < next->value)) next = &candidate;
            if (!next) break;
            auto& slot = *next; // Publish telemetry in queue order, even after slot reuse.
            collect_gpu_profile(*this, slot);
            release_uses(slot.uses, false);
            slot.objects.clear();
            complete_requests(slot.requests, ReadbackStatus::ready);
            slot.external_sync_owner.reset();
            slot.phase = SlotPhase::free;
        }
    }
    void discard_lost() noexcept
    {
        for (auto& slot : slots) if (slot.phase == SlotPhase::pending) {
            if (slot.profile) slot.profile->status = GpuProfileStatus::device_lost;
            slot.profile.reset();
            release_uses(slot.uses, false);
            slot.objects.clear();
            complete_requests(slot.requests, ReadbackStatus::device_lost);
            slot.external_sync_owner.reset();
            slot.phase = SlotPhase::free;
        }
    }
    ~QueueState()
    {
        if (submitted > completed && !owner->lost) {
            auto result = wait_value(submitted, std::numeric_limits<std::uint64_t>::max());
            if (result != VK_SUCCESS && result != VK_ERROR_DEVICE_LOST) {
                result = owner->device.logical_device().getDispatcher()->vkQueueWaitIdle(static_cast<VkQueue>(*owner->device.queue()));
            }
            if (result != VK_SUCCESS && result != VK_ERROR_DEVICE_LOST) std::terminate();
            if (result == VK_ERROR_DEVICE_LOST) owner->lost = true;
        }
        if (owner->lost) discard_lost();
        else collect(submitted); // A successful wait/queue-idle confirmed completion above.
    }
};
struct BatchState {
    BatchState(std::shared_ptr<QueueState> queue_value, std::size_t index)
        : queue(std::move(queue_value)), slot(index), uses(memory::Allocator<Use>{queue->resource}),
          objects(memory::Allocator<std::shared_ptr<ObjectState>>{queue->resource}),
          requests(memory::Allocator<std::shared_ptr<ReadbackState>>{queue->resource}) {}
    std::shared_ptr<QueueState> queue;
    std::size_t slot;
    Vector<Use> uses;
    Vector<std::shared_ptr<ObjectState>> objects;
    Vector<std::shared_ptr<ReadbackState>> requests;
    std::shared_ptr<EncoderState> encoding;
    std::shared_ptr<GpuProfileState> profile;
    std::shared_ptr<void> external_sync_owner;
    bool external_sync = false;
    std::uint64_t generation = 1;
    bool rendering = false, invalid = false;
    bool active = false;
    ~BatchState()
    {
        if (active) {
            complete_requests(requests, queue->owner->lost ? ReadbackStatus::device_lost : ReadbackStatus::cancelled);
            release_uses(uses, true);
            queue->slots[slot].phase = SlotPhase::free;
        }
    }
    Result<void> valid() const
    {
        if (!active || invalid) return std::unexpected(Error{ErrorCode::invalid_state, "command batch is not recording or was invalidated"});
        return queue->accepting();
    }
    Result<void> check(const std::shared_ptr<ResourceState>& resource) const
    {
        if (auto result = valid(); !result) return result;
        if (!resource || resource->owner != queue->owner)
            return std::unexpected(Error{ErrorCode::invalid_argument, "resource is empty or belongs to another submission queue"});
        if (resource->external_owner && resource->external_batch.lock().get() != this)
            return std::unexpected(Error{ErrorCode::invalid_state, "external image is not acquired for this batch"});
        const bool retained = std::any_of(uses.begin(), uses.end(), [&](const Use& use) { return use.resource == resource; });
        if (resource->reserved && !retained)
            return std::unexpected(Error{ErrorCode::conflict, "resource is reserved by another recording batch"});
        return {};
    }
    Result<void> retain(const std::shared_ptr<ResourceState>& resource)
    {
        if (auto result = check(resource); !result) return result;
        for (const auto& use : uses) if (use.resource == resource) return {};
        uses.emplace_back(resource, queue->resource); // Allocate before changing resource state.
        ++resource->uses;
        resource->reserved = true;
        return {};
    }
    Use* find(const std::shared_ptr<ResourceState>& resource) noexcept {
        for (auto& use : uses) if (use.resource == resource) return &use;
        return nullptr;
    }
    const vk::raii::CommandBuffer& command() const { return queue->slots[slot].command; }
    void barrier()
    {
        const auto stages = vk::PipelineStageFlagBits2::eAllCommands | vk::PipelineStageFlagBits2::eHost;
        const auto access = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite;
        const vk::MemoryBarrier2 barrier_info{stages, access, stages, access};
        vk::DependencyInfo dependency{};
        dependency.setMemoryBarriers(barrier_info);
        command().pipelineBarrier2(dependency);
        // Legacy helpers expose a conservative global barrier. Forget previous
        // typed preparations and keep both read and write scopes for later use.
        for (auto& use : uses) for (std::size_t i = 0; i < use.states.size(); ++i) {
            use.states[i].stages = stages;
            use.states[i].access = access;
            use.prepared[i] = 0;
        }
    }
    void transition(const std::shared_ptr<ResourceState>& resource, vk::ImageLayout layout)
    {
        auto& use = *find(resource);
        for (std::uint32_t layer = 0; layer < resource->image_desc.array_layers; ++layer)
        for (std::uint32_t mip = 0; mip < resource->image_desc.mip_levels; ++mip) {
        auto& prior = use.states[static_cast<std::size_t>(layer) * resource->image_desc.mip_levels + mip];
        vk::ImageMemoryBarrier2 barrier_info{};
        barrier_info.srcStageMask = prior.layout == vk::ImageLayout::eUndefined ? vk::PipelineStageFlags2{} : vk::PipelineStageFlagBits2::eAllCommands;
        barrier_info.srcAccessMask = prior.layout == vk::ImageLayout::eUndefined ? vk::AccessFlags2{} : vk::AccessFlagBits2::eMemoryWrite | vk::AccessFlagBits2::eMemoryRead;
        barrier_info.dstStageMask = vk::PipelineStageFlagBits2::eAllCommands;
        barrier_info.dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite;
        barrier_info.oldLayout = prior.layout;
        barrier_info.newLayout = layout;
        barrier_info.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier_info.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier_info.image = resource->image;
        barrier_info.subresourceRange = vk::ImageSubresourceRange{resource->image_desc.format == vk::Format::eD32Sfloat ? vk::ImageAspectFlagBits::eDepth : vk::ImageAspectFlagBits::eColor, mip, 1, layer, 1};
        vk::DependencyInfo dependency{};
        dependency.setImageMemoryBarriers(barrier_info);
        command().pipelineBarrier2(dependency);
        prior.stages = barrier_info.dstStageMask;
        prior.access = barrier_info.dstAccessMask;
        prior.layout = layout;
        use.prepared[static_cast<std::size_t>(layer) * resource->image_desc.mip_levels + mip] = 0;
        }
    }
};
} // namespace dk::graphics::detail

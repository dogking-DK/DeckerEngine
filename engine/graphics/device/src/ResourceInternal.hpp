#pragma once
#include "SubmissionInternal.hpp"
#include <algorithm>

namespace dk::graphics::detail {
struct DeviceLifetime {
    explicit DeviceLifetime(Device&& value) : device(std::move(value)) {}
    Device device;
    bool lost = false;
};
struct ResourceState {
    explicit ResourceState(std::shared_ptr<DeviceLifetime> value) : owner(std::move(value)) {}
    std::shared_ptr<DeviceLifetime> owner;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    BufferDesc buffer_desc;
    ImageDesc image_desc;
    vk::ImageLayout layout = vk::ImageLayout::eUndefined;
    std::size_t uses = 0;
    bool reserved = false;
    ~ResourceState()
    {
        if (buffer) vmaDestroyBuffer(owner->device.allocator(), buffer, allocation);
        if (image) vmaDestroyImage(owner->device.allocator(), image, allocation);
    }
};
struct Use { std::shared_ptr<ResourceState> resource; vk::ImageLayout layout; };
enum class SlotPhase { free, recording, pending };
struct Slot {
    explicit Slot(memory::ResourceHandle resource) : uses(memory::Allocator<Use>{resource}) {}
    // Command buffer must be destroyed before its pool.
    vk::raii::CommandPool pool{nullptr};
    vk::raii::CommandBuffer command{nullptr};
    Vector<Use> uses;
    SlotPhase phase = SlotPhase::free;
    std::uint64_t value = 0;
};
inline void release_uses(Vector<Use>& uses, bool recording) noexcept
{
    for (auto& use : uses) {
        --use.resource->uses;
        if (recording && use.resource->image) use.resource->reserved = false;
    }
    uses.clear();
}
struct QueueState {
    QueueState(memory::ResourceHandle resource_value, std::shared_ptr<DeviceLifetime> device_value)
        : resource(std::move(resource_value)), owner(std::move(device_value)), slots(memory::Allocator<Slot>{resource}) {}
    memory::ResourceHandle resource;
    std::shared_ptr<DeviceLifetime> owner;
    vk::raii::Semaphore timeline{nullptr};
    Vector<Slot> slots;
    SubmissionApi api;
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
        for (auto& slot : slots) if (slot.phase == SlotPhase::pending && slot.value <= completed) {
            release_uses(slot.uses, false);
            slot.phase = SlotPhase::free;
        }
    }
    void discard_lost() noexcept
    {
        for (auto& slot : slots) if (slot.phase == SlotPhase::pending) {
            release_uses(slot.uses, false);
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
        discard_lost(); // Safe after completion or terminal device loss; no fabricated completion value.
    }
};
struct BatchState {
    BatchState(std::shared_ptr<QueueState> queue_value, std::size_t index)
        : queue(std::move(queue_value)), slot(index), uses(memory::Allocator<Use>{queue->resource}) {}
    std::shared_ptr<QueueState> queue;
    std::size_t slot;
    Vector<Use> uses;
    bool active = false;
    ~BatchState()
    {
        if (active) {
            release_uses(uses, true);
            queue->slots[slot].phase = SlotPhase::free;
        }
    }
    Result<void> valid() const
    {
        if (!active) return std::unexpected(Error{ErrorCode::invalid_state, "command batch is not recording"});
        return queue->accepting();
    }
    Result<void> check(const std::shared_ptr<ResourceState>& resource) const
    {
        if (auto result = valid(); !result) return result;
        if (!resource || resource->owner != queue->owner)
            return std::unexpected(Error{ErrorCode::invalid_argument, "resource is empty or belongs to another submission queue"});
        const bool retained = std::any_of(uses.begin(), uses.end(), [&](const Use& use) { return use.resource == resource; });
        if (resource->image && resource->reserved && !retained)
            return std::unexpected(Error{ErrorCode::conflict, "image is reserved by another recording batch"});
        return {};
    }
    Result<void> retain(const std::shared_ptr<ResourceState>& resource)
    {
        if (auto result = check(resource); !result) return result;
        for (const auto& use : uses) if (use.resource == resource) return {};
        uses.push_back({resource, resource->layout}); // Allocate before changing resource state.
        ++resource->uses;
        if (resource->image) resource->reserved = true;
        return {};
    }
    const vk::raii::CommandBuffer& command() const { return queue->slots[slot].command; }
    void barrier() const
    {
        const auto stages = vk::PipelineStageFlagBits2::eAllCommands | vk::PipelineStageFlagBits2::eHost;
        const auto access = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite;
        const vk::MemoryBarrier2 barrier_info{stages, access, stages, access};
        vk::DependencyInfo dependency{};
        dependency.setMemoryBarriers(barrier_info);
        command().pipelineBarrier2(dependency);
    }
    void transition(const std::shared_ptr<ResourceState>& resource, vk::ImageLayout layout)
    {
        auto& use = *std::find_if(uses.begin(), uses.end(), [&](const Use& entry) { return entry.resource == resource; });
        vk::ImageMemoryBarrier2 barrier_info{};
        barrier_info.srcStageMask = use.layout == vk::ImageLayout::eUndefined ? vk::PipelineStageFlags2{} : vk::PipelineStageFlagBits2::eAllCommands;
        barrier_info.srcAccessMask = use.layout == vk::ImageLayout::eUndefined ? vk::AccessFlags2{} : vk::AccessFlagBits2::eMemoryWrite | vk::AccessFlagBits2::eMemoryRead;
        barrier_info.dstStageMask = vk::PipelineStageFlagBits2::eAllCommands;
        barrier_info.dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite;
        barrier_info.oldLayout = use.layout;
        barrier_info.newLayout = layout;
        barrier_info.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier_info.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier_info.image = resource->image;
        barrier_info.subresourceRange = vk::ImageSubresourceRange{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
        vk::DependencyInfo dependency{};
        dependency.setImageMemoryBarriers(barrier_info);
        command().pipelineBarrier2(dependency);
        use.layout = layout;
    }
};
} // namespace dk::graphics::detail

#include "ResourcePolicy.hpp"
#include "SubmissionInternal.hpp"
#include <dk/profiling/Profiler.hpp>
#include <algorithm>
#include <cstring>
#include <stdexcept>

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
void release_uses(Vector<Use>& uses, bool recording) noexcept
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

namespace dk::graphics {
namespace {
Error empty_batch() { return {ErrorCode::invalid_state, "command batch is empty or consumed"}; }
Result<void> host_access(const std::shared_ptr<detail::ResourceState>& state, vk::DeviceSize offset,
    std::span<std::byte> destination, std::span<const std::byte> source, bool write)
{
    if (!state) return std::unexpected(Error{ErrorCode::invalid_state, "buffer is empty"});
    if (state->owner->lost) return std::unexpected(Error{ErrorCode::invalid_state, "buffer device is lost"});
    if (state->uses) return std::unexpected(Error{ErrorCode::conflict, "buffer is retained by recorded or pending GPU work; collect before CPU access"});
    if (state->buffer_desc.memory != (write ? BufferMemory::upload : BufferMemory::readback))
        return std::unexpected(Error{ErrorCode::invalid_argument, "CPU access does not match buffer memory role"});
    const auto size = write ? source.size() : destination.size();
    if (!detail::valid_range(state->buffer_desc.size, offset, size))
        return std::unexpected(Error{ErrorCode::invalid_argument, "CPU buffer access is out of range"});
    if (!size) return {};
    const auto allocator = state->owner->device.allocator();
    const auto failure = [&](const char* operation, VkResult result) -> Result<void> {
        if (result == VK_ERROR_DEVICE_LOST) state->owner->lost = true;
        return std::unexpected(Error{ErrorCode::internal_error, std::string(operation) + " failed: " +
            std::string(vulkan_result_name(result)) + " (" + std::to_string(static_cast<int>(result)) + ")"});
    };
    void* mapped = nullptr;
    auto result = vmaMapMemory(allocator, state->allocation, &mapped);
    if (result != VK_SUCCESS) return failure("vmaMapMemory", result);
    struct Mapping { VmaAllocator allocator; VmaAllocation allocation; ~Mapping() { vmaUnmapMemory(allocator, allocation); } } mapping{allocator, state->allocation};
    auto* bytes = static_cast<std::byte*>(mapped) + offset;
    if (write) {
        std::memcpy(bytes, source.data(), size);
        result = vmaFlushAllocation(allocator, state->allocation, offset, size);
    } else {
        result = vmaInvalidateAllocation(allocator, state->allocation, offset, size);
        if (result == VK_SUCCESS) std::memcpy(destination.data(), bytes, size);
    }
    if (result != VK_SUCCESS) return failure(write ? "vmaFlushAllocation" : "vmaInvalidateAllocation", result);
    return {};
}
}
vk::Buffer Buffer::handle() const noexcept { return state_ ? vk::Buffer{state_->buffer} : vk::Buffer{}; }
vk::DeviceSize Buffer::size() const noexcept { return state_ ? state_->buffer_desc.size : 0; }
Result<void> Buffer::write(vk::DeviceSize offset, std::span<const std::byte> bytes) { return host_access(state_, offset, {}, bytes, true); }
Result<void> Buffer::read(vk::DeviceSize offset, std::span<std::byte> bytes) const { return host_access(state_, offset, bytes, {}, false); }
vk::Image Image::handle() const noexcept { return state_ ? vk::Image{state_->image} : vk::Image{}; }
ImageDesc Image::description() const noexcept { return state_ ? state_->image_desc : ImageDesc{}; }
vk::ImageLayout Image::layout() const noexcept { return state_ ? state_->layout : vk::ImageLayout::eUndefined; }
CommandBatch::CommandBatch() = default;
CommandBatch::CommandBatch(memory::UniquePtr<detail::BatchState> state) : state_(std::move(state)) {}
CommandBatch::~CommandBatch() = default;
CommandBatch::CommandBatch(CommandBatch&&) noexcept = default;
CommandBatch& CommandBatch::operator=(CommandBatch&&) noexcept = default;
Result<void> CommandBatch::retain(const Buffer& buffer) { return state_ ? state_->retain(buffer.state_) : std::unexpected(empty_batch()); }
Result<void> CommandBatch::retain(const Image& image) { return state_ ? state_->retain(image.state_) : std::unexpected(empty_batch()); }
const vk::raii::CommandBuffer& CommandBatch::command_buffer() const
{
    if (!state_ || !state_->valid()) throw std::logic_error("command buffer requires an active recording batch");
    return state_->command();
}
Result<void> CommandBatch::copy(const Buffer& source, const Buffer& destination, vk::DeviceSize size,
    vk::DeviceSize source_offset, vk::DeviceSize destination_offset)
{
    if (!state_) return std::unexpected(empty_batch());
    if (auto result = state_->check(source.state_); !result) return result;
    if (auto result = state_->check(destination.state_); !result) return result;
    if (!(source.state_->buffer_desc.usage & vk::BufferUsageFlagBits::eTransferSrc) ||
        !(destination.state_->buffer_desc.usage & vk::BufferUsageFlagBits::eTransferDst))
        return std::unexpected(Error{ErrorCode::invalid_argument, "buffer copy requires transfer source/destination usage"});
    if (auto result = detail::validate_copy(source.size(), destination.size(), size, source_offset, destination_offset, source.state_ == destination.state_); !result) return result;
    if (auto result = retain(source); !result) return result;
    if (auto result = retain(destination); !result) return result;
    state_->barrier();
    state_->command().copyBuffer(source.handle(), destination.handle(), vk::BufferCopy{source_offset, destination_offset, size});
    state_->barrier();
    return {};
}
Result<void> CommandBatch::transition(const Image& image, vk::ImageLayout layout)
{
    if (!state_) return std::unexpected(empty_batch());
    if (auto result = state_->check(image.state_); !result) return result;
    if (auto result = detail::validate_layout(layout, image.state_->image_desc.usage); !result) return result;
    if (auto result = retain(image); !result) return result;
    state_->transition(image.state_, layout);
    return {};
}
Result<void> CommandBatch::copy_to_image(const Buffer& source, const Image& destination)
{
    if (!state_) return std::unexpected(empty_batch());
    if (auto result = state_->check(source.state_); !result) return result;
    if (auto result = state_->check(destination.state_); !result) return result;
    if (!(source.state_->buffer_desc.usage & vk::BufferUsageFlagBits::eTransferSrc) ||
        !(destination.state_->image_desc.usage & vk::ImageUsageFlagBits::eTransferDst) ||
        source.size() < *detail::image_bytes(destination.description()))
        return std::unexpected(Error{ErrorCode::invalid_argument, "buffer/image upload size or usage mismatch"});
    if (auto result = retain(source); !result) return result;
    if (auto result = transition(destination, vk::ImageLayout::eTransferDstOptimal); !result) return result;
    state_->barrier();
    vk::BufferImageCopy region{};
    region.imageSubresource = vk::ImageSubresourceLayers{vk::ImageAspectFlagBits::eColor, 0, 0, 1};
    region.imageExtent = vk::Extent3D{destination.description().width, destination.description().height, 1};
    state_->command().copyBufferToImage(source.handle(), destination.handle(), vk::ImageLayout::eTransferDstOptimal, region);
    return {};
}
Result<void> CommandBatch::copy_to_buffer(const Image& source, const Buffer& destination)
{
    if (!state_) return std::unexpected(empty_batch());
    if (auto result = state_->check(source.state_); !result) return result;
    if (auto result = state_->check(destination.state_); !result) return result;
    if (!(source.state_->image_desc.usage & vk::ImageUsageFlagBits::eTransferSrc) ||
        !(destination.state_->buffer_desc.usage & vk::BufferUsageFlagBits::eTransferDst) ||
        destination.size() < *detail::image_bytes(source.description()))
        return std::unexpected(Error{ErrorCode::invalid_argument, "image/buffer readback size or usage mismatch"});
    if (auto result = retain(destination); !result) return result;
    if (auto result = transition(source, vk::ImageLayout::eTransferSrcOptimal); !result) return result;
    state_->barrier();
    vk::BufferImageCopy region{};
    region.imageSubresource = vk::ImageSubresourceLayers{vk::ImageAspectFlagBits::eColor, 0, 0, 1};
    region.imageExtent = vk::Extent3D{source.description().width, source.description().height, 1};
    state_->command().copyImageToBuffer(source.handle(), vk::ImageLayout::eTransferSrcOptimal, destination.handle(), region);
    state_->barrier();
    return {};
}

Result<SubmissionQueue> SubmissionQueue::create(memory::ResourceHandle resource, Device&& device, std::uint32_t slots)
{ return detail::SubmissionAccess::create(std::move(resource), std::move(device), slots); }
Result<SubmissionQueue> detail::SubmissionAccess::create(memory::ResourceHandle resource, Device&& device, std::uint32_t slots, SubmissionApi api)
{
    if (!resource || resource.state() != memory::ResourceState::open || slots == 0 || slots > 64)
        return std::unexpected(Error{ErrorCode::invalid_argument, "submission queue requires open Memory resource and 1..64 slots"});
    auto owner = memory::make_shared_in<DeviceLifetime>(resource, std::move(device));
    auto state = memory::make_shared_in<QueueState>(resource, resource, std::move(owner));
    const auto& logical = state->owner->device.logical_device();
    if (!api.submit) api.submit = logical.getDispatcher()->vkQueueSubmit2;
    if (!api.counter) api.counter = logical.getDispatcher()->vkGetSemaphoreCounterValue;
    if (!api.wait) api.wait = logical.getDispatcher()->vkWaitSemaphores;
    if (!api.submit || !api.counter || !api.wait)
        return std::unexpected(Error{ErrorCode::not_supported, "submission requires synchronization2 and timeline entry points"});
    state->api = api;
    try {
        vk::SemaphoreTypeCreateInfo type{vk::SemaphoreType::eTimeline, 0};
        vk::SemaphoreCreateInfo semaphore{};
        semaphore.pNext = &type;
        state->timeline = vk::raii::Semaphore{logical, semaphore};
        state->slots.reserve(slots);
        for (std::uint32_t i = 0; i < slots; ++i) {
            state->slots.emplace_back(resource);
            auto& slot = state->slots.back();
            slot.pool = vk::raii::CommandPool{logical,
                vk::CommandPoolCreateInfo{vk::CommandPoolCreateFlagBits::eTransient, state->owner->device.queue_family()}};
            auto buffers = logical.allocateCommandBuffers(vk::CommandBufferAllocateInfo{*slot.pool, vk::CommandBufferLevel::ePrimary, 1});
            slot.command = std::move(buffers.front());
        }
    } catch (const vk::SystemError& error) {
        return std::unexpected(state->failure("create submission resources", static_cast<VkResult>(error.code().value())));
    }
    return SubmissionQueue{std::move(state)};
}
SubmissionQueue::~SubmissionQueue() = default;
SubmissionQueue::SubmissionQueue(SubmissionQueue&&) noexcept = default;
SubmissionQueue& SubmissionQueue::operator=(SubmissionQueue&&) noexcept = default;
const Device& SubmissionQueue::device() const noexcept { return state_->owner->device; }
Result<Buffer> SubmissionQueue::create_buffer(const BufferDesc& description)
{
    DK_PROFILE_ZONE("graphics.buffer.create");
    if (auto result = state_->accepting(); !result) return std::unexpected(result.error());
    if (auto result = detail::validate_buffer(description); !result) return std::unexpected(result.error());
    auto resource = memory::make_shared_in<detail::ResourceState>(state_->resource, state_->owner);
    resource->buffer_desc = description;
    vk::BufferCreateInfo info{{}, description.size, description.usage, vk::SharingMode::eExclusive};
    VmaAllocationCreateInfo allocation{};
    allocation.usage = description.memory == BufferMemory::device ? VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE : VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
    if (description.memory == BufferMemory::upload) allocation.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
    if (description.memory == BufferMemory::readback) allocation.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;
    const auto result = vmaCreateBuffer(device().allocator(), reinterpret_cast<const VkBufferCreateInfo*>(&info), &allocation,
        &resource->buffer, &resource->allocation, nullptr);
    if (result != VK_SUCCESS) {
        resource->buffer = VK_NULL_HANDLE;
        return std::unexpected(state_->failure("vmaCreateBuffer", result));
    }
    return Buffer{std::move(resource)};
}
Result<Image> SubmissionQueue::create_image(const ImageDesc& description)
{
    DK_PROFILE_ZONE("graphics.image.create");
    if (auto result = state_->accepting(); !result) return std::unexpected(result.error());
    if (auto result = detail::image_bytes(description); !result) return std::unexpected(result.error());
    try {
        const auto properties = device().physical_device().getImageFormatProperties(description.format,
            vk::ImageType::e2D, vk::ImageTiling::eOptimal, description.usage, {});
        if (description.width > properties.maxExtent.width || description.height > properties.maxExtent.height)
            return std::unexpected(Error{ErrorCode::not_supported, "image extent exceeds device format limits"});
    } catch (const vk::SystemError& error) {
        return std::unexpected(state_->failure("vkGetPhysicalDeviceImageFormatProperties", static_cast<VkResult>(error.code().value())));
    }
    auto resource = memory::make_shared_in<detail::ResourceState>(state_->resource, state_->owner);
    resource->image_desc = description;
    vk::ImageCreateInfo info{{}, vk::ImageType::e2D, description.format, vk::Extent3D{description.width, description.height, 1},
        1, 1, vk::SampleCountFlagBits::e1, vk::ImageTiling::eOptimal, description.usage, vk::SharingMode::eExclusive};
    VmaAllocationCreateInfo allocation{};
    allocation.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    const auto result = vmaCreateImage(device().allocator(), reinterpret_cast<const VkImageCreateInfo*>(&info), &allocation,
        &resource->image, &resource->allocation, nullptr);
    if (result != VK_SUCCESS) {
        resource->image = VK_NULL_HANDLE;
        return std::unexpected(state_->failure("vmaCreateImage", result));
    }
    return Image{std::move(resource)};
}
Result<CommandBatch> SubmissionQueue::begin()
{
    DK_PROFILE_ZONE("graphics.commands.begin");
    if (auto result = state_->accepting(); !result) return std::unexpected(result.error());
    // Collection is explicit: a frame boundary alone does not recycle a slot.
    for (std::size_t index = 0; index < state_->slots.size(); ++index) {
        auto& slot = state_->slots[index];
        if (slot.phase != detail::SlotPhase::free) continue;
        auto batch = memory::make_unique_in<detail::BatchState>(state_->resource, state_, index);
        try {
            slot.pool.reset({});
            slot.command.begin(vk::CommandBufferBeginInfo{vk::CommandBufferUsageFlagBits::eOneTimeSubmit});
        } catch (const vk::SystemError& error) {
            return std::unexpected(state_->failure("begin command batch", static_cast<VkResult>(error.code().value())));
        }
        batch->active = true;
        slot.phase = detail::SlotPhase::recording;
        return CommandBatch{std::move(batch)};
    }
    return std::unexpected(Error{ErrorCode::conflict, "all submission slots are recording or pending; wait/poll before reuse"});
}
Result<Submission> SubmissionQueue::submit(CommandBatch&& batch)
{
    DK_PROFILE_ZONE("graphics.submit");
    if (auto result = state_->accepting(); !result) return std::unexpected(result.error());
    if (!batch.state_ || batch.state_->queue != state_ || !batch.state_->active)
        return std::unexpected(Error{ErrorCode::invalid_argument, "submission requires an active batch from this queue"});
    if (state_->submitted == std::numeric_limits<std::uint64_t>::max())
        return std::unexpected(Error{ErrorCode::invalid_state, "timeline value exhausted"});
    auto recording = std::move(batch.state_);
    auto& slot = state_->slots[recording->slot];
    try { slot.command.end(); }
    catch (const vk::SystemError& error) {
        return std::unexpected(state_->failure("vkEndCommandBuffer", static_cast<VkResult>(error.code().value())));
    }
    Submission ticket;
    ticket.owner_ = state_;
    ticket.value_ = state_->submitted + 1;
    const vk::CommandBufferSubmitInfo command{*slot.command};
    const vk::SemaphoreSubmitInfo signal{*state_->timeline, ticket.value_, vk::PipelineStageFlagBits2::eAllCommands};
    vk::SubmitInfo2 submit{};
    submit.setCommandBufferInfos(command).setSignalSemaphoreInfos(signal);
    const auto result = state_->api.submit(static_cast<VkQueue>(*device().queue()), 1,
        reinterpret_cast<const VkSubmitInfo2*>(&submit), VK_NULL_HANDLE);
    if (result != VK_SUCCESS) return std::unexpected(state_->failure("vkQueueSubmit2", result));
    // Commit: all ownership transfers below are noexcept and require no allocation.
    static_assert(noexcept(slot.uses = std::move(recording->uses)));
    slot.uses = std::move(recording->uses);
    for (auto& use : slot.uses) if (use.resource->image) {
        use.resource->layout = use.layout;
        use.resource->reserved = false;
    }
    state_->submitted = ticket.value_;
    slot.value = ticket.value_;
    slot.phase = detail::SlotPhase::pending;
    recording->active = false;
    return ticket;
}
Result<void> SubmissionQueue::poll()
{
    DK_PROFILE_ZONE("graphics.collect");
    if (state_->owner->lost) return std::unexpected(Error{ErrorCode::invalid_state, "submission device is lost"});
    std::uint64_t value = 0;
    const auto result = state_->api.counter(device().native_device(), static_cast<VkSemaphore>(*state_->timeline), &value);
    if (result != VK_SUCCESS) return std::unexpected(state_->failure("vkGetSemaphoreCounterValue", result));
    if (value > state_->submitted) return std::unexpected(Error{ErrorCode::internal_error, "timeline advanced beyond submitted work"});
    state_->collect(value);
    return {};
}
Result<bool> SubmissionQueue::wait(const Submission& submission, std::uint64_t timeout_ns)
{
    DK_PROFILE_ZONE("graphics.wait");
    if (submission.owner_.lock() != state_ || !submission.value_ || submission.value_ > state_->submitted)
        return std::unexpected(Error{ErrorCode::invalid_argument, "completion ticket does not belong to this queue"});
    if (state_->owner->lost) return std::unexpected(Error{ErrorCode::invalid_state, "submission device is lost"});
    if (submission.value_ <= state_->completed) return true;
    const auto result = state_->wait_value(submission.value_, timeout_ns);
    if (result == VK_TIMEOUT) return false;
    if (result != VK_SUCCESS) return std::unexpected(state_->failure("vkWaitSemaphores", result));
    state_->collect(submission.value_);
    return true;
}
Result<void> SubmissionQueue::close()
{
    if (state_->closed) return {};
    for (const auto& slot : state_->slots) if (slot.phase == detail::SlotPhase::recording)
        return std::unexpected(Error{ErrorCode::conflict, "abandon or submit recording batches before closing"});
    if (state_->owner->lost) {
        state_->discard_lost();
        state_->closed = true;
        return std::unexpected(Error{ErrorCode::invalid_state, "closed a lost submission device"});
    }
    if (state_->submitted > state_->completed) {
        const auto result = state_->wait_value(state_->submitted, std::numeric_limits<std::uint64_t>::max());
        if (result != VK_SUCCESS) return std::unexpected(state_->failure("close submission queue", result));
        state_->collect(state_->submitted);
    }
    state_->closed = true;
    return {};
}
SubmissionStats SubmissionQueue::stats() const noexcept
{
    SubmissionStats result;
    result.submitted = state_->submitted;
    result.completed = state_->completed;
    result.closed = state_->closed;
    result.device_lost = state_->owner->lost;
    for (const auto& slot : state_->slots) {
        if (slot.phase == detail::SlotPhase::free) ++result.free_slots;
        if (slot.phase == detail::SlotPhase::recording) ++result.recording_slots;
        if (slot.phase == detail::SlotPhase::pending) ++result.pending_slots;
        result.retained_resources += slot.uses.size();
    }
    return result;
}
} // namespace dk::graphics

#pragma once
#include <dk/graphics/Device.hpp>
#include <cstddef>
#include <limits>

namespace dk::graphics {
enum class BufferMemory { device, upload, readback };
struct BufferDesc {
    vk::DeviceSize size = 0;
    vk::BufferUsageFlags usage = vk::BufferUsageFlagBits::eTransferSrc | vk::BufferUsageFlagBits::eTransferDst;
    BufferMemory memory = BufferMemory::device;
};
struct ImageDesc {
    std::uint32_t width = 0, height = 0;
    vk::Format format = vk::Format::eR8G8B8A8Unorm;
    vk::ImageUsageFlags usage = vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled;
};
namespace detail { struct ResourceState; struct QueueState; struct BatchState; struct SubmissionAccess; }
class CommandBatch;
class SubmissionQueue;
class ResourceFactory;

// VMA owners. Native handles are borrowed; submitted batches retain allocations.
// All accesses to a queue and its resources must be externally serialized.
class Buffer final {
public:
    Buffer() = default;
    Buffer(Buffer&&) noexcept = default;
    Buffer& operator=(Buffer&&) noexcept = default;
    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;
    [[nodiscard]] explicit operator bool() const noexcept { return bool(state_); }
    [[nodiscard]] vk::Buffer handle() const noexcept;
    [[nodiscard]] vk::DeviceSize size() const noexcept;
    [[nodiscard]] Result<void> write(vk::DeviceSize offset, std::span<const std::byte> bytes);
    [[nodiscard]] Result<void> read(vk::DeviceSize offset, std::span<std::byte> bytes) const;
private:
    friend class SubmissionQueue;
    friend class CommandBatch;
    friend class ResourceFactory;
    explicit Buffer(std::shared_ptr<detail::ResourceState> state) : state_(std::move(state)) {}
    std::shared_ptr<detail::ResourceState> state_;
};
class Image final {
public:
    Image() = default;
    Image(Image&&) noexcept = default;
    Image& operator=(Image&&) noexcept = default;
    Image(const Image&) = delete;
    Image& operator=(const Image&) = delete;
    [[nodiscard]] explicit operator bool() const noexcept { return bool(state_); }
    [[nodiscard]] vk::Image handle() const noexcept;
    [[nodiscard]] ImageDesc description() const noexcept;
    // Last successfully submitted layout; may still be in flight on this queue.
    [[nodiscard]] vk::ImageLayout layout() const noexcept;
private:
    friend class SubmissionQueue;
    friend class CommandBatch;
    friend class ResourceFactory;
    explicit Image(std::shared_ptr<detail::ResourceState> state) : state_(std::move(state)) {}
    std::shared_ptr<detail::ResourceState> state_;
};
class Submission final {
public:
    Submission() = default;
    [[nodiscard]] std::uint64_t value() const noexcept { return value_; }
private:
    friend class SubmissionQueue;
    std::weak_ptr<detail::QueueState> owner_;
    std::uint64_t value_ = 0;
};
class CommandBatch final {
public:
    CommandBatch();
    ~CommandBatch(); // Abandons unsubmitted work and releases recording reservations.
    CommandBatch(CommandBatch&&) noexcept;
    CommandBatch& operator=(CommandBatch&&) noexcept;
    CommandBatch(const CommandBatch&) = delete;
    CommandBatch& operator=(const CommandBatch&) = delete;
    [[nodiscard]] Result<void> retain(const Buffer& buffer);
    [[nodiscard]] Result<void> retain(const Image& image);
    [[nodiscard]] Result<void> copy(const Buffer& source, const Buffer& destination,
        vk::DeviceSize size, vk::DeviceSize source_offset = 0, vk::DeviceSize destination_offset = 0);
    [[nodiscard]] Result<void> copy_to_image(const Buffer& source, const Image& destination);
    [[nodiscard]] Result<void> copy_to_buffer(const Image& source, const Buffer& destination);
    [[nodiscard]] Result<void> transition(const Image& image, vk::ImageLayout layout);
    // Valid only during this batch. Retain every manually referenced resource.
    // Do not end/submit/reset the command buffer or bypass tracked image layouts.
    [[nodiscard]] const vk::raii::CommandBuffer& command_buffer() const;
private:
    friend class SubmissionQueue;
    explicit CommandBatch(memory::UniquePtr<detail::BatchState> state);
    memory::UniquePtr<detail::BatchState> state_;
};
struct SubmissionStats {
    std::uint64_t submitted = 0, completed = 0;
    std::uint32_t free_slots = 0, recording_slots = 0, pending_slots = 0;
    std::size_t retained_resources = 0; // References retained by pending slots.
    bool closed = false, device_lost = false;
};
class SubmissionQueue final {
public:
    // Consumes Device. Buffer/Image keep the device alive beyond this queue.
    [[nodiscard]] static Result<SubmissionQueue> create(memory::ResourceHandle resource, Device&& device, std::uint32_t slots = 3);
    ~SubmissionQueue(); // Drains submitted work before destroying pools/semaphore.
    // Moved-from queues may only be destroyed or assigned to.
    SubmissionQueue(SubmissionQueue&&) noexcept;
    SubmissionQueue& operator=(SubmissionQueue&&) noexcept;
    SubmissionQueue(const SubmissionQueue&) = delete;
    SubmissionQueue& operator=(const SubmissionQueue&) = delete;
    [[nodiscard]] const Device& device() const noexcept;
    [[nodiscard]] ResourceFactory resources() const noexcept;
    [[nodiscard]] Result<Buffer> create_buffer(const BufferDesc& description);
    [[nodiscard]] Result<Image> create_image(const ImageDesc& description);
    [[nodiscard]] Result<CommandBatch> begin();
    // Consumes a valid local batch on success or driver failure, never publishes on failure.
    [[nodiscard]] Result<Submission> submit(CommandBatch&& batch);
    [[nodiscard]] Result<void> poll();
    [[nodiscard]] Result<bool> wait(const Submission& submission, std::uint64_t timeout_ns = std::numeric_limits<std::uint64_t>::max());
    [[nodiscard]] Result<void> close();
    [[nodiscard]] SubmissionStats stats() const noexcept;
private:
    friend struct detail::SubmissionAccess;
    friend class ResourceFactory;
    explicit SubmissionQueue(std::shared_ptr<detail::QueueState> state) : state_(std::move(state)) {}
    std::shared_ptr<detail::QueueState> state_;
};
} // namespace dk::graphics

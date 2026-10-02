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
    std::uint32_t mip_levels = 1, array_layers = 1;
};
struct AccessState {
    vk::PipelineStageFlags2 stages{};
    vk::AccessFlags2 access{};
    vk::ImageLayout layout = vk::ImageLayout::eUndefined;
    bool initialized = false;
    bool operator==(const AccessState&) const = default;
};
namespace detail { struct ResourceState; struct QueueState; struct BatchState; struct SubmissionAccess; struct ObjectAccess; }
class CommandBatch;
class SubmissionQueue;
class ResourceFactory;
class RenderEncoder;
class ComputeEncoder;
class ImageView;
class Sampler;
class ShaderModule;
class PipelineLayout;
class ComputePipeline;
class GraphicsPipeline;
class BindingSet;
struct ResourceUse;
struct ResourceBarrier;
struct RenderingDesc;
struct ImageCopyRegion;

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
    friend struct detail::ObjectAccess;
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
    // Last successfully submitted states, ordered by layer then mip; not completion.
    [[nodiscard]] Result<AccessState> state(std::uint32_t mip = 0, std::uint32_t layer = 0) const;
private:
    friend class SubmissionQueue;
    friend class CommandBatch;
    friend class ResourceFactory;
    friend struct detail::ObjectAccess;
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
    [[nodiscard]] Result<void> retain(const ImageView& view);
    [[nodiscard]] Result<void> retain(const Sampler& sampler);
    [[nodiscard]] Result<void> retain(const ShaderModule& shader);
    [[nodiscard]] Result<void> retain(const PipelineLayout& layout);
    [[nodiscard]] Result<void> retain(const ComputePipeline& pipeline);
    [[nodiscard]] Result<void> retain(const GraphicsPipeline& pipeline);
    [[nodiscard]] Result<void> retain(const BindingSet& bindings);
    [[nodiscard]] Result<void> copy(const Buffer& source, const Buffer& destination,
        vk::DeviceSize size, vk::DeviceSize source_offset = 0, vk::DeviceSize destination_offset = 0);
    [[nodiscard]] Result<void> copy_to_image(const Buffer& source, const Image& destination);
    [[nodiscard]] Result<void> copy_to_buffer(const Image& source, const Buffer& destination);
    [[nodiscard]] Result<void> transition(const Image& image, vk::ImageLayout layout);
    [[nodiscard]] Result<void> prepare(std::span<const ResourceUse> uses);
    [[nodiscard]] Result<void> barrier(std::span<const ResourceBarrier> barriers);
    [[nodiscard]] Result<RenderEncoder> begin_rendering(const RenderingDesc& description);
    [[nodiscard]] Result<ComputeEncoder> compute();
    [[nodiscard]] Result<void> copy_buffer(const Buffer& source, const Buffer& destination,
        vk::DeviceSize size, vk::DeviceSize source_offset = 0, vk::DeviceSize destination_offset = 0);
    [[nodiscard]] Result<void> copy_to_image(const Buffer& source, const Image& destination, const ImageCopyRegion& region);
    [[nodiscard]] Result<void> copy_to_buffer(const Image& source, const Buffer& destination, const ImageCopyRegion& region);
    [[nodiscard]] Result<void> fill(const Buffer& buffer, std::uint32_t value = 0);
    [[nodiscard]] Result<void> clear(const Image& image, const vk::ClearColorValue& color, const vk::ImageSubresourceRange& range);
    using NativeRecorder = void (*)(const vk::raii::CommandBuffer&, void*);
    [[nodiscard]] Result<void> unsafe_record(std::span<const ResourceUse> before, std::span<const ResourceUse> after,
        NativeRecorder recorder, void* user_data = nullptr);
    // Valid only during this batch. Retain every manually referenced resource.
    // Do not end/submit/reset the command buffer or bypass tracked image layouts.
    [[nodiscard]] const vk::raii::CommandBuffer& command_buffer() const;
private:
    friend class SubmissionQueue;
    friend struct detail::ObjectAccess;
    explicit CommandBatch(std::shared_ptr<detail::BatchState> state);
    std::shared_ptr<detail::BatchState> state_;
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

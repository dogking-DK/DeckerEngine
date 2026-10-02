#pragma once
#include "ResourceInternal.hpp"
#include <dk/graphics/GpuObjects.hpp>
#include <dk/graphics/Bindings.hpp>

namespace dk::graphics::detail {
struct ObjectState {
    explicit ObjectState(std::shared_ptr<DeviceLifetime> device) : owner(std::move(device)) {}
    std::shared_ptr<DeviceLifetime> owner;
};
struct ViewState : ObjectState {
    ViewState(std::shared_ptr<DeviceLifetime> device, std::shared_ptr<ResourceState> resource, ImageViewDesc desc)
        : ObjectState(std::move(device)), image(std::move(resource)), description(desc) {}
    std::shared_ptr<ResourceState> image;
    ImageViewDesc description;
    vk::raii::ImageView view{nullptr};
};
struct SamplerState : ObjectState {
    using ObjectState::ObjectState;
    vk::raii::Sampler sampler{nullptr};
};
struct ReflectedBinding {
    std::uint32_t set, binding;
    ShaderDescriptorType type;
    std::uint32_t count, block_size;
};
struct ShaderState : ObjectState {
    ShaderState(std::shared_ptr<DeviceLifetime> device, memory::ResourceHandle resource)
        : ObjectState(std::move(device)), entry(memory::Allocator<char>{resource}),
          bindings(memory::Allocator<ReflectedBinding>{resource}), push(memory::Allocator<ShaderPushConstant>{resource}) {}
    String entry;
    ShaderStage stage = ShaderStage::compute;
    std::array<std::uint32_t, 3> group{};
    Vector<ReflectedBinding> bindings;
    Vector<ShaderPushConstant> push;
    vk::raii::ShaderModule shader{nullptr};
};
struct ObjectAccess {
    static ResourceFactory factory(const BatchState& batch) { return ResourceFactory{batch.queue}; }
    static const auto& state(const ImageView& value) { return value.state_; }
    static const auto& state(const Sampler& value) { return value.state_; }
    static const auto& state(const ShaderModule& value) { return value.state_; }
    static const auto& state(const PipelineLayout& value) { return value.state_; }
    static const auto& state(const ComputePipeline& value) { return value.state_; }
    static const auto& state(const GraphicsPipeline& value) { return value.state_; }
    static const auto& state(const BindingSet& value) { return value.state_; }
    static const auto& state(const Buffer& value) { return value.state_; }
    static const auto& state(const Image& value) { return value.state_; }
    static const auto& state(const CommandBatch& value) { return value.state_; }
    static void fail_creation(const ResourceFactory& factory, VkResult result) {
        if (auto queue = factory.queue_.lock()) queue->object_creation_failure = result;
    }
};
struct LayoutState : ObjectState {
    LayoutState(std::shared_ptr<DeviceLifetime> device, memory::ResourceHandle resource)
        : ObjectState(std::move(device)), bindings(memory::Allocator<LayoutBinding>{resource}),
          push(memory::Allocator<vk::PushConstantRange>{resource}), sets(memory::Allocator<vk::raii::DescriptorSetLayout>{resource}) {}
    Vector<LayoutBinding> bindings;
    Vector<vk::PushConstantRange> push;
    Vector<vk::raii::DescriptorSetLayout> sets;
    vk::raii::PipelineLayout layout{nullptr};
};
struct PipelineState : ObjectState {
    PipelineState(std::shared_ptr<DeviceLifetime> device, memory::ResourceHandle resource, std::shared_ptr<LayoutState> layout_value)
        : ObjectState(std::move(device)), layout(std::move(layout_value)),
          vertex_bindings(memory::Allocator<vk::VertexInputBindingDescription>{resource}),
          vertex_attributes(memory::Allocator<vk::VertexInputAttributeDescription>{resource}) {}
    std::shared_ptr<LayoutState> layout;
    vk::PipelineBindPoint point = vk::PipelineBindPoint::eCompute;
    std::array<std::uint32_t, 3> group{};
    vk::Format color_format = vk::Format::eUndefined, depth_format = vk::Format::eUndefined;
    vk::PrimitiveTopology topology = vk::PrimitiveTopology::eTriangleList;
    Vector<vk::VertexInputBindingDescription> vertex_bindings;
    Vector<vk::VertexInputAttributeDescription> vertex_attributes;
    vk::raii::Pipeline pipeline{nullptr};
};
struct PoolPage : ObjectState {
    using ObjectState::ObjectState;
    std::uint32_t allocations = 0;
    std::array<std::uint32_t, 5> counts{};
    vk::raii::DescriptorPool pool{nullptr};
};
struct BoundResource {
    LayoutBinding binding;
    std::uint32_t element = 0;
    std::shared_ptr<ResourceState> buffer;
    std::shared_ptr<ViewState> view;
    std::shared_ptr<SamplerState> sampler;
    vk::DeviceSize offset = 0, size = 0;
    vk::ImageLayout image_layout = vk::ImageLayout::eUndefined;
};
struct BindingState : ObjectState {
    BindingState(std::shared_ptr<DeviceLifetime> device, memory::ResourceHandle resource)
        : ObjectState(std::move(device)), resources(memory::Allocator<BoundResource>{resource}) {}
    std::shared_ptr<LayoutState> layout;
    std::shared_ptr<PoolPage> page;
    Vector<BoundResource> resources;
    std::uint32_t set_index = 0;
    vk::raii::DescriptorSet set{nullptr};
    ~BindingState() { set.clear(); if (page) --page->allocations; }
};
[[nodiscard]] bool compatible_layouts(const LayoutState& left, const LayoutState& right, std::uint32_t set) noexcept;
[[nodiscard]] vk::ShaderStageFlagBits native_stage(ShaderStage stage) noexcept;
[[nodiscard]] vk::DescriptorType native_descriptor(ShaderDescriptorType type) noexcept;
inline Result<void> object_creation_status(QueueState& queue, const char* operation)
{
    const auto result = std::exchange(queue.object_creation_failure, VK_SUCCESS);
    if (result != VK_SUCCESS) return std::unexpected(queue.failure(operation, result));
    return {};
}
inline Result<std::shared_ptr<QueueState>> factory_queue(const std::weak_ptr<QueueState>& weak)
{
    auto queue = weak.lock();
    if (!queue) return std::unexpected(Error{ErrorCode::invalid_state, "resource factory queue has expired"});
    if (auto result = queue->accepting(); !result) return std::unexpected(result.error());
    if (queue->resource.state() != memory::ResourceState::open)
        return std::unexpected(Error{ErrorCode::invalid_state, "resource factory Memory is closing or closed"});
    return queue;
}
} // namespace dk::graphics::detail

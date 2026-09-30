#pragma once
#include "ResourceInternal.hpp"
#include <dk/graphics/GpuObjects.hpp>

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
    static const auto& state(const ImageView& value) { return value.state_; }
    static const auto& state(const Sampler& value) { return value.state_; }
    static const auto& state(const ShaderModule& value) { return value.state_; }
    static void fail_creation(const ResourceFactory& factory, VkResult result) {
        if (auto queue = factory.queue_.lock()) queue->object_creation_failure = result;
    }
};
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

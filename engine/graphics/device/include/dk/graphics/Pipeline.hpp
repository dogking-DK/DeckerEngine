#pragma once
#include <dk/graphics/GpuObjects.hpp>

namespace dk::graphics {
namespace detail { struct LayoutState; struct PipelineState; }
struct LayoutBinding {
    std::uint32_t set = 0, binding = 0;
    ShaderDescriptorType type = ShaderDescriptorType::storage_buffer;
    std::uint32_t count = 1, minimum_buffer_size = 0;
    vk::ShaderStageFlags stages{};
};
class PipelineLayout final {
public:
    PipelineLayout() = default;
    PipelineLayout(PipelineLayout&&) noexcept = default;
    PipelineLayout& operator=(PipelineLayout&&) noexcept = default;
    PipelineLayout(const PipelineLayout&) = delete;
    PipelineLayout& operator=(const PipelineLayout&) = delete;
    [[nodiscard]] explicit operator bool() const noexcept { return bool(state_); }
    [[nodiscard]] vk::PipelineLayout handle() const noexcept;
    [[nodiscard]] std::span<const LayoutBinding> bindings() const noexcept;
    [[nodiscard]] std::span<const vk::PushConstantRange> push_constants() const noexcept;
    [[nodiscard]] bool compatible_for_set(const PipelineLayout& other, std::uint32_t set) const noexcept;
private:
    friend class ResourceFactory;
    friend struct detail::ObjectAccess;
    explicit PipelineLayout(std::shared_ptr<detail::LayoutState> state) : state_(std::move(state)) {}
    std::shared_ptr<detail::LayoutState> state_;
};
struct ComputePipelineDesc {
    const ShaderModule* shader = nullptr;
    const PipelineLayout* layout = nullptr;
};
struct GraphicsPipelineDesc {
    const ShaderModule* vertex = nullptr;
    const ShaderModule* fragment = nullptr;
    const PipelineLayout* layout = nullptr;
    std::span<const vk::VertexInputBindingDescription> vertex_bindings{};
    std::span<const vk::VertexInputAttributeDescription> vertex_attributes{};
    vk::PrimitiveTopology topology = vk::PrimitiveTopology::eTriangleList;
    vk::CullModeFlags cull_mode = vk::CullModeFlagBits::eNone;
    vk::FrontFace front_face = vk::FrontFace::eCounterClockwise;
    vk::Format color_format = vk::Format::eR8G8B8A8Unorm;
    vk::Format depth_format = vk::Format::eUndefined;
    bool depth_test = false, depth_write = false, blend = false;
    vk::CompareOp depth_compare = vk::CompareOp::eLess;
};
class ComputePipeline final {
public:
    ComputePipeline() = default;
    ComputePipeline(ComputePipeline&&) noexcept = default;
    ComputePipeline& operator=(ComputePipeline&&) noexcept = default;
    ComputePipeline(const ComputePipeline&) = delete;
    ComputePipeline& operator=(const ComputePipeline&) = delete;
    [[nodiscard]] explicit operator bool() const noexcept { return bool(state_); }
    [[nodiscard]] vk::Pipeline handle() const noexcept;
    [[nodiscard]] std::array<std::uint32_t, 3> thread_group_size() const noexcept;
private:
    friend class ResourceFactory;
    friend struct detail::ObjectAccess;
    explicit ComputePipeline(std::shared_ptr<detail::PipelineState> state) : state_(std::move(state)) {}
    std::shared_ptr<detail::PipelineState> state_;
};
class GraphicsPipeline final {
public:
    GraphicsPipeline() = default;
    GraphicsPipeline(GraphicsPipeline&&) noexcept = default;
    GraphicsPipeline& operator=(GraphicsPipeline&&) noexcept = default;
    GraphicsPipeline(const GraphicsPipeline&) = delete;
    GraphicsPipeline& operator=(const GraphicsPipeline&) = delete;
    [[nodiscard]] explicit operator bool() const noexcept { return bool(state_); }
    [[nodiscard]] vk::Pipeline handle() const noexcept;
private:
    friend class ResourceFactory;
    friend struct detail::ObjectAccess;
    explicit GraphicsPipeline(std::shared_ptr<detail::PipelineState> state) : state_(std::move(state)) {}
    std::shared_ptr<detail::PipelineState> state_;
};
} // namespace dk::graphics

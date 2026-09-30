#pragma once
#include <dk/graphics/Resources.hpp>
#include <dk/graphics/ShaderArtifact.hpp>

namespace dk::graphics {
class PipelineLayout;
class GraphicsPipeline;
class ComputePipeline;
class BindingSet;
struct GraphicsPipelineDesc;
struct ComputePipelineDesc;
struct BindingWrite;
namespace detail { struct ViewState; struct SamplerState; struct ShaderState; struct ObjectAccess; }
struct ImageViewDesc {
    vk::ImageViewType type = vk::ImageViewType::e2D;
    vk::ImageSubresourceRange range{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
};
struct SamplerDesc {
    vk::Filter min_filter = vk::Filter::eLinear, mag_filter = vk::Filter::eLinear;
    vk::SamplerMipmapMode mipmap_mode = vk::SamplerMipmapMode::eLinear;
    vk::SamplerAddressMode address_u = vk::SamplerAddressMode::eRepeat;
    vk::SamplerAddressMode address_v = vk::SamplerAddressMode::eRepeat;
    vk::SamplerAddressMode address_w = vk::SamplerAddressMode::eRepeat;
    float min_lod = 0, max_lod = 0;
};
class ImageView final {
public:
    ImageView() = default;
    ImageView(ImageView&&) noexcept = default;
    ImageView& operator=(ImageView&&) noexcept = default;
    ImageView(const ImageView&) = delete;
    ImageView& operator=(const ImageView&) = delete;
    [[nodiscard]] explicit operator bool() const noexcept { return bool(state_); }
    [[nodiscard]] vk::ImageView handle() const noexcept;
    [[nodiscard]] ImageViewDesc description() const noexcept;
private:
    friend class ResourceFactory;
    friend struct detail::ObjectAccess;
    explicit ImageView(std::shared_ptr<detail::ViewState> state) : state_(std::move(state)) {}
    std::shared_ptr<detail::ViewState> state_;
};
class Sampler final {
public:
    Sampler() = default;
    Sampler(Sampler&&) noexcept = default;
    Sampler& operator=(Sampler&&) noexcept = default;
    Sampler(const Sampler&) = delete;
    Sampler& operator=(const Sampler&) = delete;
    [[nodiscard]] explicit operator bool() const noexcept { return bool(state_); }
    [[nodiscard]] vk::Sampler handle() const noexcept;
private:
    friend class ResourceFactory;
    friend struct detail::ObjectAccess;
    explicit Sampler(std::shared_ptr<detail::SamplerState> state) : state_(std::move(state)) {}
    std::shared_ptr<detail::SamplerState> state_;
};
class ShaderModule final {
public:
    ShaderModule() = default;
    ShaderModule(ShaderModule&&) noexcept = default;
    ShaderModule& operator=(ShaderModule&&) noexcept = default;
    ShaderModule(const ShaderModule&) = delete;
    ShaderModule& operator=(const ShaderModule&) = delete;
    [[nodiscard]] explicit operator bool() const noexcept { return bool(state_); }
    [[nodiscard]] vk::ShaderModule handle() const noexcept;
    [[nodiscard]] ShaderStage stage() const noexcept;
    [[nodiscard]] std::string_view entry() const noexcept;
private:
    friend class ResourceFactory;
    friend struct detail::ObjectAccess;
    explicit ShaderModule(std::shared_ptr<detail::ShaderState> state) : state_(std::move(state)) {}
    std::shared_ptr<detail::ShaderState> state_;
};
// Borrowing facade; an expired/closed queue is rejected without native calls.
class ResourceFactory final {
public:
    ResourceFactory() = default;
    [[nodiscard]] Result<Buffer> create_buffer(const BufferDesc& description) const;
    [[nodiscard]] Result<Image> create_image(const ImageDesc& description) const;
    [[nodiscard]] Result<ImageView> create_view(const Image& image, const ImageViewDesc& description = {}) const;
    [[nodiscard]] Result<Sampler> create_sampler(const SamplerDesc& description = {}) const;
    [[nodiscard]] Result<ShaderModule> create_shader(const CompiledShader& shader) const;
    [[nodiscard]] Result<PipelineLayout> create_pipeline_layout(std::span<const ShaderModule* const> shaders) const;
    [[nodiscard]] Result<GraphicsPipeline> create_graphics_pipeline(const GraphicsPipelineDesc& description) const;
    [[nodiscard]] Result<ComputePipeline> create_compute_pipeline(const ComputePipelineDesc& description) const;
    [[nodiscard]] Result<BindingSet> create_bindings(const PipelineLayout& layout, std::uint32_t set,
        std::span<const BindingWrite> writes) const;
private:
    friend class SubmissionQueue;
    friend struct detail::ObjectAccess;
    explicit ResourceFactory(const std::shared_ptr<detail::QueueState>& queue) : queue_(queue) {}
    std::weak_ptr<detail::QueueState> queue_;
};
} // namespace dk::graphics

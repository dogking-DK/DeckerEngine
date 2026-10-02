#pragma once
#include <dk/graphics/Bindings.hpp>

namespace dk::graphics {
struct ResourceUse {
    const Buffer* buffer = nullptr;
    const Image* image = nullptr;
    AccessState state{};
    vk::DeviceSize offset = 0, size = VK_WHOLE_SIZE;
    vk::ImageSubresourceRange range{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
    bool full_overwrite = false; // Caller guarantees the next shader write initializes the entire declared subresource.
};
struct ResourceBarrier { ResourceUse resource; AccessState before; };
[[nodiscard]] ResourceUse buffer_use(const Buffer& buffer, vk::PipelineStageFlags2 stages, vk::AccessFlags2 access,
                                    vk::DeviceSize offset = 0, vk::DeviceSize size = VK_WHOLE_SIZE);
[[nodiscard]] ResourceUse image_use(const Image& image, vk::PipelineStageFlags2 stages, vk::AccessFlags2 access,
    vk::ImageLayout layout, vk::ImageSubresourceRange range = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1});
struct ImageCopyRegion {
    std::uint32_t mip = 0, layer = 0, x = 0, y = 0, width = 0, height = 0;
    vk::DeviceSize buffer_offset = 0;
    std::uint32_t row_length = 0, image_height = 0; // In texels; zero means tightly packed.
};
struct ColorAttachment {
    const ImageView* view = nullptr;
    vk::AttachmentLoadOp load = vk::AttachmentLoadOp::eClear;
    vk::AttachmentStoreOp store = vk::AttachmentStoreOp::eStore;
    std::array<float,4> clear{0,0,0,1};
};
struct DepthAttachment {
    const ImageView* view = nullptr;
    vk::AttachmentLoadOp load = vk::AttachmentLoadOp::eClear;
    vk::AttachmentStoreOp store = vk::AttachmentStoreOp::eStore;
    float clear = 1;
};
struct RenderingDesc {
    ColorAttachment color;
    DepthAttachment depth;
    vk::Rect2D area{}; // Zero extent selects the full attachment mip.
};
class ComputeEncoder final {
public:
    ComputeEncoder() = default;
    [[nodiscard]] Result<void> bind_pipeline(const ComputePipeline& pipeline);
    [[nodiscard]] Result<void> bind_sets(std::span<const BindingSet* const> sets);
    [[nodiscard]] Result<void> push_constants(vk::ShaderStageFlags stages, std::uint32_t offset, std::span<const std::byte> bytes);
    [[nodiscard]] Result<void> dispatch(std::array<std::uint32_t,3> groups);
private:
    friend class CommandBatch;
    ComputeEncoder(const std::shared_ptr<detail::BatchState>& batch, std::uint64_t generation) : batch_(batch), generation_(generation) {}
    std::weak_ptr<detail::BatchState> batch_;
    std::uint64_t generation_ = 0;
};
class RenderEncoder final {
public:
    RenderEncoder() = default;
    ~RenderEncoder();
    RenderEncoder(RenderEncoder&& other) noexcept;
    RenderEncoder& operator=(RenderEncoder&& other) noexcept;
    RenderEncoder(const RenderEncoder&) = delete;
    RenderEncoder& operator=(const RenderEncoder&) = delete;
    [[nodiscard]] Result<void> bind_pipeline(const GraphicsPipeline& pipeline);
    [[nodiscard]] Result<void> bind_sets(std::span<const BindingSet* const> sets);
    [[nodiscard]] Result<void> push_constants(vk::ShaderStageFlags stages, std::uint32_t offset, std::span<const std::byte> bytes);
    [[nodiscard]] Result<void> vertex_buffer(std::uint32_t binding, const Buffer& buffer, vk::DeviceSize offset = 0);
    [[nodiscard]] Result<void> index_buffer(const Buffer& buffer, vk::IndexType type, vk::DeviceSize offset = 0);
    [[nodiscard]] Result<void> viewport(const vk::Viewport& viewport);
    [[nodiscard]] Result<void> scissor(const vk::Rect2D& scissor);
    [[nodiscard]] Result<void> draw(std::uint32_t vertices, std::uint32_t instances = 1, std::uint32_t first_vertex = 0, std::uint32_t first_instance = 0);
    [[nodiscard]] Result<void> draw_indexed(std::uint32_t indices, std::uint32_t instances = 1, std::uint32_t first_index = 0,
        std::int32_t vertex_offset = 0, std::uint32_t first_instance = 0);
    [[nodiscard]] Result<void> end();
private:
    friend class CommandBatch;
    RenderEncoder(const std::shared_ptr<detail::BatchState>& batch, std::uint64_t generation) : batch_(batch), generation_(generation) {}
    std::weak_ptr<detail::BatchState> batch_;
    std::uint64_t generation_ = 0;
};
} // namespace dk::graphics

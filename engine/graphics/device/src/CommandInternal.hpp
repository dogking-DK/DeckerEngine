#pragma once
#include "ObjectInternal.hpp"
#include "ResourcePolicy.hpp"
#include <dk/graphics/CommandEncoder.hpp>

namespace dk::graphics::detail {
inline constexpr auto write_access = vk::AccessFlagBits2::eShaderWrite | vk::AccessFlagBits2::eShaderStorageWrite |
    vk::AccessFlagBits2::eTransferWrite | vk::AccessFlagBits2::eColorAttachmentWrite |
    vk::AccessFlagBits2::eDepthStencilAttachmentWrite | vk::AccessFlagBits2::eHostWrite | vk::AccessFlagBits2::eMemoryWrite;
inline constexpr auto read_access = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderSampledRead |
    vk::AccessFlagBits2::eShaderStorageRead | vk::AccessFlagBits2::eUniformRead | vk::AccessFlagBits2::eTransferRead |
    vk::AccessFlagBits2::eVertexAttributeRead | vk::AccessFlagBits2::eIndexRead |
    vk::AccessFlagBits2::eColorAttachmentRead | vk::AccessFlagBits2::eDepthStencilAttachmentRead |
    vk::AccessFlagBits2::eHostRead | vk::AccessFlagBits2::eMemoryRead;
struct ResolvedUse {
    std::shared_ptr<ResourceState> resource;
    AccessState state;
    vk::DeviceSize offset, size;
    vk::ImageSubresourceRange range;
    bool full_overwrite = false;
};
struct RetainRollback {
    explicit RetainRollback(BatchState& value) : batch(value), count(value.uses.size()), objects(value.objects.size()) {}
    BatchState& batch;
    std::size_t count, objects;
    bool committed = false;
    ~RetainRollback() {
        if (committed) return;
        while (batch.uses.size() > count) {
            auto& use = batch.uses.back();
            --use.resource->uses;
            use.resource->reserved = false;
            batch.uses.pop_back();
        }
        batch.objects.resize(objects);
    }
};
struct VertexSlot { std::uint32_t binding; std::shared_ptr<ResourceState> buffer; vk::DeviceSize offset; };
struct EncoderState {
    explicit EncoderState(memory::ResourceHandle resource)
        : sets(memory::Allocator<std::shared_ptr<BindingState>>{resource}),
          push_written(memory::Allocator<vk::ShaderStageFlags>{resource}), vertices(memory::Allocator<VertexSlot>{resource}) {}
    std::shared_ptr<PipelineState> pipeline;
    Vector<std::shared_ptr<BindingState>> sets;
    Vector<vk::ShaderStageFlags> push_written;
    Vector<VertexSlot> vertices;
    std::shared_ptr<ResourceState> index;
    vk::DeviceSize index_offset = 0;
    vk::IndexType index_type = vk::IndexType::eUint32;
    std::shared_ptr<ViewState> color, depth;
    vk::AttachmentStoreOp color_store = vk::AttachmentStoreOp::eStore, depth_store = vk::AttachmentStoreOp::eStore;
    vk::Rect2D area{};
};
Result<void> outside_rendering(const BatchState& batch);
Result<ResolvedUse> resolve_use(BatchState& batch, const ResourceUse& use);
Result<void> prepare_resolved(BatchState& batch, std::span<const ResolvedUse> uses, std::span<const AccessState> before = {});
bool stage_covers(vk::PipelineStageFlags2 actual, vk::PipelineStageFlags2 requested) noexcept;
bool access_covers(vk::AccessFlags2 actual, vk::AccessFlags2 requested) noexcept;
Result<void> require_use(BatchState& batch, const std::shared_ptr<ResourceState>& resource, vk::PipelineStageFlags2 stages,
    vk::AccessFlags2 access, const vk::ImageSubresourceRange* range = nullptr, vk::ImageLayout layout = vk::ImageLayout::eUndefined,
    bool require_content = true);
void consume_use(BatchState& batch, const std::shared_ptr<ResourceState>& resource, const vk::ImageSubresourceRange* range = nullptr,
                 bool initializes = false) noexcept;
Result<void> retain_object(BatchState& batch, const std::shared_ptr<ObjectState>& object);
void invalidate_encoder(BatchState& batch) noexcept;
struct CopyFootprint { vk::BufferImageCopy native; vk::ImageSubresourceRange range; vk::DeviceSize bytes; bool full; };
Result<CopyFootprint> copy_footprint(const ImageDesc& image, const ImageCopyRegion& region);
} // namespace dk::graphics::detail

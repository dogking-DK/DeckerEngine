#include "CommandInternal.hpp"
#include "PipelinePolicy.hpp"
#include <cmath>

namespace dk::graphics {
namespace {
using namespace detail;
Error invalid(const char* text) { return {ErrorCode::invalid_argument, text}; }
Error bad_state(const char* text) { return {ErrorCode::invalid_state, text}; }
Result<std::shared_ptr<BatchState>> encoder(const std::weak_ptr<BatchState>& weak, std::uint64_t generation, bool render)
{
    auto batch = weak.lock();
    if (!batch || batch->generation != generation || batch->rendering != render || !batch->encoding)
        return std::unexpected(bad_state("encoder has ended, moved, or expired"));
    if (auto valid = batch->valid(); !valid) return std::unexpected(valid.error());
    return batch;
}
std::shared_ptr<EncoderState> new_encoding(BatchState& batch)
{
    auto result = memory::make_shared_in<EncoderState>(batch.queue->resource, batch.queue->resource);
    const auto& limits = batch.queue->owner->device.adapter().properties.limits;
    result->sets.resize(limits.maxBoundDescriptorSets);
    result->push_written.resize(limits.maxPushConstantsSize);
    result->vertices.reserve(limits.maxVertexInputBindings);
    return result;
}
Result<void> bind_pipeline(BatchState& batch, const std::shared_ptr<PipelineState>& pipeline, bool render)
{
    if (!pipeline || pipeline->owner != batch.queue->owner || pipeline->point != (render ? vk::PipelineBindPoint::eGraphics : vk::PipelineBindPoint::eCompute))
        return std::unexpected(invalid("pipeline is empty, foreign, or has the wrong bind point"));
    auto& state = *batch.encoding;
    if (render && (pipeline->color_format != state.color->image->image_desc.format ||
        pipeline->depth_format != (state.depth ? state.depth->image->image_desc.format : vk::Format::eUndefined)))
        return std::unexpected(invalid("pipeline attachment formats do not match rendering"));
    if (auto valid = retain_object(batch, pipeline); !valid) return valid;
    if (state.pipeline && state.pipeline->layout != pipeline->layout) {
        for (std::uint32_t i = 0; i < state.sets.size(); ++i)
            if (state.sets[i] && !compatible_layouts(*state.sets[i]->layout, *pipeline->layout, i)) state.sets[i].reset();
        if (state.pipeline->layout->push != pipeline->layout->push)
            std::fill(state.push_written.begin(), state.push_written.end(), vk::ShaderStageFlags{});
    }
    state.pipeline = pipeline;
    batch.command().bindPipeline(pipeline->point, *pipeline->pipeline);
    return {};
}
Result<void> retain_binding(BatchState& batch, const std::shared_ptr<BindingState>& set)
{
    if (auto valid = retain_object(batch, set); !valid) return valid;
    for (const auto& resource : set->resources) {
        if (resource.buffer) { if (auto valid = batch.retain(resource.buffer); !valid) return valid; }
        if (resource.view) { if (auto valid = batch.retain(resource.view->image); !valid) return valid; }
    }
    return {};
}
Result<void> bind_sets(BatchState& batch, std::span<const BindingSet* const> inputs)
{
    auto& state = *batch.encoding;
    if (!state.pipeline) return std::unexpected(bad_state("bind a pipeline before descriptor sets"));
    RetainRollback rollback{batch};
    for (std::size_t i = 0; i < inputs.size(); ++i) {
        if (!inputs[i] || !ObjectAccess::state(*inputs[i])) return std::unexpected(invalid("empty descriptor set"));
        const auto& set = ObjectAccess::state(*inputs[i]);
        if (!compatible_layouts(*set->layout, *state.pipeline->layout, set->set_index))
            return std::unexpected(invalid("descriptor set layout is incompatible with pipeline"));
        for (std::size_t j = 0; j < i; ++j) if (inputs[j]->set_index() == set->set_index)
            return std::unexpected(invalid("duplicate descriptor set index"));
        if (auto valid = retain_binding(batch, set); !valid) return valid;
    }
    for (const auto* input : inputs) {
        const auto& set = ObjectAccess::state(*input);
        batch.command().bindDescriptorSets(state.pipeline->point, *state.pipeline->layout->layout, set->set_index, *set->set, {});
        state.sets[set->set_index] = set;
    }
    rollback.committed = true;
    return {};
}
Result<void> push_constants(BatchState& batch, vk::ShaderStageFlags stages, std::uint32_t offset, std::span<const std::byte> bytes)
{
    auto& state = *batch.encoding;
    if (!state.pipeline) return std::unexpected(bad_state("bind a pipeline before push constants"));
    const auto allowed = batch.rendering ? vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment : vk::ShaderStageFlags{vk::ShaderStageFlagBits::eCompute};
    if (!stages || (stages & ~allowed) || bytes.empty() || bytes.size() % 4 || offset % 4 || !valid_range(state.push_written.size(), offset, bytes.size()))
        return std::unexpected(invalid("push constant range/stages invalid"));
    for (std::size_t i = offset; i < offset + bytes.size(); ++i) {
        vk::ShaderStageFlags covered{};
        for (const auto& range : state.pipeline->layout->push) if (i >= range.offset && i - range.offset < range.size) covered |= range.stageFlags;
        // Vulkan requires all stages in overlapping ranges for each updated byte.
        if ((covered & stages) != stages || (covered & ~stages)) return std::unexpected(invalid("push constant update does not match layout stage coverage"));
    }
    batch.command().pushConstants<std::byte>(*state.pipeline->layout->layout, stages, offset, bytes);
    for (std::size_t i = offset; i < offset + bytes.size(); ++i) state.push_written[i] |= stages;
    return {};
}
vk::PipelineStageFlags2 shader_stages(vk::ShaderStageFlags stages)
{
    vk::PipelineStageFlags2 result{};
    if (stages & vk::ShaderStageFlagBits::eVertex) result |= vk::PipelineStageFlagBits2::eVertexShader;
    if (stages & vk::ShaderStageFlagBits::eFragment) result |= vk::PipelineStageFlagBits2::eFragmentShader;
    if (stages & vk::ShaderStageFlagBits::eCompute) result |= vk::PipelineStageFlagBits2::eComputeShader;
    return result;
}
Result<void> validate_bindings(BatchState& batch)
{
    const auto& state = *batch.encoding;
    if (!state.pipeline) return std::unexpected(bad_state("draw/dispatch requires a pipeline"));
    const auto active = batch.rendering ? vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment : vk::ShaderStageFlags{vk::ShaderStageFlagBits::eCompute};
    for (const auto& push : state.pipeline->layout->push)
        for (std::uint32_t i = push.offset; i < push.offset + push.size; ++i)
            if ((state.push_written[i] & (push.stageFlags & active)) != (push.stageFlags & active))
                return std::unexpected(bad_state("required push constant bytes are unset"));
    for (const auto& binding : state.pipeline->layout->bindings) {
        if (!(binding.stages & active)) continue;
        const auto& set = state.sets[binding.set];
        if (!set) return std::unexpected(bad_state("required descriptor set is unset"));
        for (const auto& bound : set->resources) {
            if (bound.binding.binding != binding.binding || bound.sampler) continue;
            const auto resource = bound.buffer ? bound.buffer : bound.view->image;
            const auto* range = bound.view ? &bound.view->description.range : nullptr;
            vk::AccessFlags2 access{};
            if (binding.type == ShaderDescriptorType::uniform_buffer) {
                if (bound.size < binding.minimum_buffer_size) return std::unexpected(invalid("compatible layout uniform range does not cover this pipeline's block"));
                access = vk::AccessFlagBits2::eUniformRead;
            }
            else if (binding.type == ShaderDescriptorType::sampled_image) access = vk::AccessFlagBits2::eShaderSampledRead;
            else {
                auto* use = batch.find(resource);
                if (!use) return std::unexpected(bad_state("storage binding must be prepared with explicit read/write intent"));
                const auto index = range ? static_cast<std::size_t>(range->baseArrayLayer) * resource->image_desc.mip_levels + range->baseMipLevel : 0;
                access = use->states[index].access & (vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite | vk::AccessFlagBits2::eShaderStorageRead | vk::AccessFlagBits2::eShaderStorageWrite);
                if (!access || (batch.rendering && (access & write_access)))
                    return std::unexpected(bad_state("storage requires explicit shader access; graphics storage writes are unsupported"));
            }
            if (batch.rendering && bound.view && (resource == state.color->image || (state.depth && resource == state.depth->image)))
                return std::unexpected(bad_state("attachment feedback is unsupported"));
            if (auto valid = require_use(batch, resource, shader_stages(binding.stages & active), access, range, bound.image_layout); !valid) return valid;
        }
    }
    return {};
}
void consume_bindings(BatchState& batch) noexcept
{
    for (const auto& set : batch.encoding->sets) if (set)
        for (const auto& bound : set->resources) {
            if (bound.buffer) consume_use(batch, bound.buffer);
            if (bound.view) consume_use(batch, bound.view->image, &bound.view->description.range);
        }
}
Result<void> validate_vertices(BatchState& batch, std::uint32_t count, std::uint32_t instances, std::uint32_t first, std::uint32_t first_instance, bool indexed)
{
    const auto& state = *batch.encoding;
    for (const auto& binding : state.pipeline->vertex_bindings) {
        const auto found = std::find_if(state.vertices.begin(), state.vertices.end(), [&](const auto& slot) { return slot.binding == binding.binding; });
        if (found == state.vertices.end()) return std::unexpected(bad_state("required vertex binding is unset"));
        if (auto valid = require_use(batch, found->buffer, vk::PipelineStageFlagBits2::eVertexAttributeInput, vk::AccessFlagBits2::eVertexAttributeRead); !valid) return valid;
        std::uint64_t last = 0;
        if (binding.inputRate == vk::VertexInputRate::eInstance) last = std::uint64_t{first_instance} + instances - 1;
        else if (!indexed) last = std::uint64_t{first} + count - 1;
        // Index contents remain caller-owned; only the minimum vertex extent is provable without reading GPU memory.
        for (const auto& attribute : state.pipeline->vertex_attributes) if (attribute.binding == binding.binding) {
            const auto offset = last * binding.stride + attribute.offset;
            if (!valid_range(found->buffer->buffer_desc.size - found->offset, offset, vertex_format_size(attribute.format)))
                return std::unexpected(invalid("vertex/instance input range exceeds buffer"));
        }
    }
    return {};
}
bool valid_ops(vk::AttachmentLoadOp load, vk::AttachmentStoreOp store)
{ return (load == vk::AttachmentLoadOp::eLoad || load == vk::AttachmentLoadOp::eClear || load == vk::AttachmentLoadOp::eDontCare) &&
         (store == vk::AttachmentStoreOp::eStore || store == vk::AttachmentStoreOp::eDontCare); }
Result<void> attachment(BatchState& batch, const std::shared_ptr<ViewState>& view, bool depth, vk::AttachmentLoadOp load)
{
    if (!view || view->description.type != vk::ImageViewType::e2D || view->description.range.levelCount != 1 || view->description.range.layerCount != 1 ||
        (view->image->image_desc.format == vk::Format::eD32Sfloat) != depth)
        return std::unexpected(invalid("attachment requires a matching single mip/layer 2D view"));
    auto access = depth ? vk::AccessFlagBits2::eDepthStencilAttachmentWrite | vk::AccessFlagBits2::eDepthStencilAttachmentRead :
        vk::AccessFlags2{vk::AccessFlagBits2::eColorAttachmentWrite};
    if (!depth) access |= vk::AccessFlagBits2::eColorAttachmentRead; // Includes blend reads.
    const auto stages = depth ? vk::PipelineStageFlagBits2::eEarlyFragmentTests | vk::PipelineStageFlagBits2::eLateFragmentTests :
        vk::PipelineStageFlags2{vk::PipelineStageFlagBits2::eColorAttachmentOutput};
    const auto layout = depth ? vk::ImageLayout::eDepthStencilAttachmentOptimal : vk::ImageLayout::eColorAttachmentOptimal;
    if (auto valid = require_use(batch, view->image, stages, access, &view->description.range, layout, load == vk::AttachmentLoadOp::eLoad); !valid) return valid;
    return retain_object(batch, view);
}
void attachment_content(BatchState& batch, const std::shared_ptr<ViewState>& view, vk::AttachmentLoadOp load, bool full)
{
    auto* use = batch.find(view->image);
    const auto& range = view->description.range;
    auto& content = use->states[static_cast<std::size_t>(range.baseArrayLayer) * view->image->image_desc.mip_levels + range.baseMipLevel].initialized;
    if (load == vk::AttachmentLoadOp::eDontCare) content = false;
    else if (load == vk::AttachmentLoadOp::eClear && full) content = true;
}
} // namespace

Result<void> CommandBatch::retain(const ImageView& value)
{
    if (!state_) return std::unexpected(bad_state("empty batch"));
    const auto& view = ObjectAccess::state(value);
    RetainRollback rollback{*state_};
    if (auto valid = retain_object(*state_,view); !valid) return valid;
    if (auto valid = state_->retain(view->image); !valid) return valid;
    rollback.committed = true;
    return {};
}
Result<void> CommandBatch::retain(const Sampler& value)
{ return state_ ? retain_object(*state_,ObjectAccess::state(value)) : std::unexpected(bad_state("empty batch")); }
Result<void> CommandBatch::retain(const ShaderModule& value)
{ return state_ ? retain_object(*state_,ObjectAccess::state(value)) : std::unexpected(bad_state("empty batch")); }
Result<void> CommandBatch::retain(const PipelineLayout& value)
{ return state_ ? retain_object(*state_,ObjectAccess::state(value)) : std::unexpected(bad_state("empty batch")); }
Result<void> CommandBatch::retain(const ComputePipeline& value)
{ return state_ ? retain_object(*state_,ObjectAccess::state(value)) : std::unexpected(bad_state("empty batch")); }
Result<void> CommandBatch::retain(const GraphicsPipeline& value)
{ return state_ ? retain_object(*state_,ObjectAccess::state(value)) : std::unexpected(bad_state("empty batch")); }
Result<void> CommandBatch::retain(const BindingSet& value)
{
    if (!state_) return std::unexpected(bad_state("empty batch"));
    RetainRollback rollback{*state_};
    if (auto valid = retain_binding(*state_,ObjectAccess::state(value)); !valid) return valid;
    rollback.committed = true;
    return {};
}
Result<ComputeEncoder> CommandBatch::compute()
{
    if (!state_) return std::unexpected(bad_state("empty batch"));
    if (auto valid = outside_rendering(*state_); !valid) return std::unexpected(valid.error());
    auto encoding = new_encoding(*state_);
    invalidate_encoder(*state_);
    state_->encoding = std::move(encoding);
    return ComputeEncoder{state_, state_->generation};
}
Result<RenderEncoder> CommandBatch::begin_rendering(const RenderingDesc& desc)
{
    if (!state_) return std::unexpected(bad_state("empty batch"));
    if (auto valid = outside_rendering(*state_); !valid) return std::unexpected(valid.error());
    if (!desc.color.view || !valid_ops(desc.color.load, desc.color.store) || !valid_ops(desc.depth.load, desc.depth.store) ||
        !std::isfinite(desc.depth.clear) || desc.depth.clear < 0 || desc.depth.clear > 1)
        return std::unexpected(invalid("rendering attachment/load/store invalid"));
    auto encoding = new_encoding(*state_);
    encoding->color = ObjectAccess::state(*desc.color.view);
    if (desc.depth.view) encoding->depth = ObjectAccess::state(*desc.depth.view);
    RetainRollback rollback{*state_};
    if (auto valid = attachment(*state_, encoding->color, false, desc.color.load); !valid) return std::unexpected(valid.error());
    if (desc.depth.view) if (auto valid = attachment(*state_, encoding->depth, true, desc.depth.load); !valid) return std::unexpected(valid.error());
    const auto& image = encoding->color->image->image_desc;
    const auto mip = encoding->color->description.range.baseMipLevel;
    const auto width = std::max(1u, image.width >> mip), height = std::max(1u, image.height >> mip);
    auto area = desc.area;
    if (!area.extent.width && !area.extent.height) area = vk::Rect2D{{0,0},{width,height}};
    if (area.offset.x < 0 || area.offset.y < 0 || !area.extent.width || !area.extent.height ||
        !valid_range(width, static_cast<std::uint32_t>(area.offset.x), area.extent.width) || !valid_range(height, static_cast<std::uint32_t>(area.offset.y), area.extent.height))
        return std::unexpected(invalid("render area exceeds attachment"));
    if (encoding->depth) {
        const auto& depth = encoding->depth->image->image_desc;
        const auto dmip = encoding->depth->description.range.baseMipLevel;
        if (width != std::max(1u, depth.width >> dmip) || height != std::max(1u, depth.height >> dmip))
            return std::unexpected(invalid("color/depth attachment extents differ"));
    }
    const auto& limits = state_->queue->owner->device.adapter().properties.limits;
    if (width > limits.maxFramebufferWidth || height > limits.maxFramebufferHeight || width > limits.maxViewportDimensions[0] || height > limits.maxViewportDimensions[1])
        return std::unexpected(invalid("attachment extent exceeds rendering limits"));
    encoding->area = area;
    encoding->color_store = desc.color.store;
    encoding->depth_store = desc.depth.store;
    vk::RenderingAttachmentInfo color{};
    color.setImageView(*encoding->color->view).setImageLayout(vk::ImageLayout::eColorAttachmentOptimal)
        .setLoadOp(desc.color.load).setStoreOp(desc.color.store).setClearValue(vk::ClearValue{vk::ClearColorValue{desc.color.clear}});
    vk::RenderingAttachmentInfo depth{};
    if (encoding->depth) depth.setImageView(*encoding->depth->view).setImageLayout(vk::ImageLayout::eDepthStencilAttachmentOptimal)
        .setLoadOp(desc.depth.load).setStoreOp(desc.depth.store).setClearValue(vk::ClearValue{vk::ClearDepthStencilValue{desc.depth.clear,0}});
    vk::RenderingInfo info{};
    info.setRenderArea(area).setLayerCount(1).setColorAttachments(color).setPDepthAttachment(encoding->depth ? &depth : nullptr);
    state_->command().beginRendering(info);
    state_->command().setViewport(0, vk::Viewport{static_cast<float>(area.offset.x),static_cast<float>(area.offset.y),static_cast<float>(area.extent.width),static_cast<float>(area.extent.height),0,1});
    state_->command().setScissor(0, area);
    const bool full = area.offset.x == 0 && area.offset.y == 0 && area.extent.width == width && area.extent.height == height;
    attachment_content(*state_, encoding->color, desc.color.load, full);
    if (encoding->depth) attachment_content(*state_, encoding->depth, desc.depth.load, full);
    invalidate_encoder(*state_);
    state_->encoding = std::move(encoding);
    state_->rendering = true;
    rollback.committed = true;
    return RenderEncoder{state_, state_->generation};
}
Result<void> ComputeEncoder::bind_pipeline(const ComputePipeline& value)
{ auto batch = encoder(batch_,generation_,false); return batch ? graphics::bind_pipeline(**batch,ObjectAccess::state(value),false) : std::unexpected(batch.error()); }
Result<void> RenderEncoder::bind_pipeline(const GraphicsPipeline& value)
{ auto batch = encoder(batch_,generation_,true); return batch ? graphics::bind_pipeline(**batch,ObjectAccess::state(value),true) : std::unexpected(batch.error()); }
Result<void> ComputeEncoder::bind_sets(std::span<const BindingSet* const> sets)
{ auto batch = encoder(batch_,generation_,false); return batch ? graphics::bind_sets(**batch,sets) : std::unexpected(batch.error()); }
Result<void> RenderEncoder::bind_sets(std::span<const BindingSet* const> sets)
{ auto batch = encoder(batch_,generation_,true); return batch ? graphics::bind_sets(**batch,sets) : std::unexpected(batch.error()); }
Result<void> ComputeEncoder::push_constants(vk::ShaderStageFlags stages, std::uint32_t offset, std::span<const std::byte> bytes)
{ auto batch = encoder(batch_,generation_,false); return batch ? graphics::push_constants(**batch,stages,offset,bytes) : std::unexpected(batch.error()); }
Result<void> RenderEncoder::push_constants(vk::ShaderStageFlags stages, std::uint32_t offset, std::span<const std::byte> bytes)
{ auto batch = encoder(batch_,generation_,true); return batch ? graphics::push_constants(**batch,stages,offset,bytes) : std::unexpected(batch.error()); }
Result<void> ComputeEncoder::dispatch(std::array<std::uint32_t,3> groups)
{
    auto owner = encoder(batch_,generation_,false);
    if (!owner) return std::unexpected(owner.error());
    auto& batch = **owner;
    const auto& limits = batch.queue->owner->device.adapter().properties.limits;
    for (std::size_t i = 0; i < 3; ++i) if (!groups[i] || groups[i] > limits.maxComputeWorkGroupCount[i]) return std::unexpected(invalid("dispatch group count exceeds limits"));
    if (auto valid = validate_bindings(batch); !valid) return valid;
    batch.command().dispatch(groups[0],groups[1],groups[2]);
    consume_bindings(batch);
    return {};
}
RenderEncoder::~RenderEncoder() { if (auto batch = batch_.lock(); batch && batch->generation == generation_ && batch->rendering && batch->valid()) (void)end(); }
RenderEncoder::RenderEncoder(RenderEncoder&& other) noexcept : batch_(std::move(other.batch_)), generation_(std::exchange(other.generation_,0)) {}
RenderEncoder& RenderEncoder::operator=(RenderEncoder&& other) noexcept
{
    if (this != &other) { if (auto batch = batch_.lock(); batch && batch->generation == generation_ && batch->rendering && batch->valid()) (void)end();
        batch_ = std::move(other.batch_); generation_ = std::exchange(other.generation_,0); }
    return *this;
}
Result<void> RenderEncoder::vertex_buffer(std::uint32_t binding, const Buffer& buffer, vk::DeviceSize offset)
{
    auto owner = encoder(batch_,generation_,true);
    if (!owner) return std::unexpected(owner.error());
    auto& batch = **owner;
    const auto& resource = ObjectAccess::state(buffer);
    if (!resource || binding >= batch.queue->owner->device.adapter().properties.limits.maxVertexInputBindings || offset >= buffer.size() || !(resource->buffer_desc.usage & vk::BufferUsageFlagBits::eVertexBuffer))
        return std::unexpected(invalid("vertex buffer binding/offset/usage invalid"));
    if (auto valid = batch.retain(resource); !valid) return valid;
    auto& vertices = batch.encoding->vertices;
    auto found = std::find_if(vertices.begin(), vertices.end(), [&](const auto& slot) { return slot.binding == binding; });
    if (found == vertices.end()) vertices.push_back({binding,resource,offset}); else *found = {binding,resource,offset};
    batch.command().bindVertexBuffers(binding, buffer.handle(), offset);
    return {};
}
Result<void> RenderEncoder::index_buffer(const Buffer& buffer, vk::IndexType type, vk::DeviceSize offset)
{
    auto owner = encoder(batch_,generation_,true);
    if (!owner) return std::unexpected(owner.error());
    auto& batch = **owner;
    const auto& resource = ObjectAccess::state(buffer);
    if (!resource || (type != vk::IndexType::eUint16 && type != vk::IndexType::eUint32) || offset >= buffer.size() ||
        offset % (type == vk::IndexType::eUint16 ? 2 : 4) || !(resource->buffer_desc.usage & vk::BufferUsageFlagBits::eIndexBuffer))
        return std::unexpected(invalid("index buffer type/offset/usage invalid"));
    if (auto valid = batch.retain(resource); !valid) return valid;
    batch.encoding->index = resource;
    batch.encoding->index_offset = offset;
    batch.encoding->index_type = type;
    batch.command().bindIndexBuffer(buffer.handle(),offset,type);
    return {};
}
Result<void> RenderEncoder::viewport(const vk::Viewport& value)
{
    auto owner = encoder(batch_,generation_,true);
    if (!owner) return std::unexpected(owner.error());
    auto& batch = **owner;
    const auto& limits = batch.queue->owner->device.adapter().properties.limits;
    if (!std::isfinite(value.x) || !std::isfinite(value.y) || !std::isfinite(value.width) || !std::isfinite(value.height) ||
        !std::isfinite(value.minDepth) || !std::isfinite(value.maxDepth) || value.width <= 0 || value.height <= 0 ||
        value.width > limits.maxViewportDimensions[0] || value.height > limits.maxViewportDimensions[1] ||
        value.x < limits.viewportBoundsRange[0] || value.y < limits.viewportBoundsRange[0] ||
        value.x + value.width > limits.viewportBoundsRange[1] || value.y + value.height > limits.viewportBoundsRange[1] ||
        value.minDepth < 0 || value.maxDepth > 1 || value.minDepth > value.maxDepth)
        return std::unexpected(invalid("viewport exceeds supported positive extent/depth bounds"));
    batch.command().setViewport(0,value);
    return {};
}
Result<void> RenderEncoder::scissor(const vk::Rect2D& value)
{
    auto owner = encoder(batch_,generation_,true);
    if (!owner) return std::unexpected(owner.error());
    const auto& area = (*owner)->encoding->area;
    if (value.offset.x < area.offset.x || value.offset.y < area.offset.y || !value.extent.width || !value.extent.height ||
        !valid_range(area.extent.width, static_cast<std::uint32_t>(value.offset.x-area.offset.x), value.extent.width) ||
        !valid_range(area.extent.height, static_cast<std::uint32_t>(value.offset.y-area.offset.y), value.extent.height))
        return std::unexpected(invalid("scissor exceeds render area"));
    (*owner)->command().setScissor(0,value);
    return {};
}
Result<void> RenderEncoder::draw(std::uint32_t vertices, std::uint32_t instances, std::uint32_t first_vertex, std::uint32_t first_instance)
{
    auto owner = encoder(batch_,generation_,true);
    if (!owner) return std::unexpected(owner.error());
    if (!vertices || !instances) return std::unexpected(invalid("draw counts must be nonzero"));
    auto& batch = **owner;
    if (auto valid = validate_bindings(batch); !valid) return valid;
    if (auto valid = validate_vertices(batch,vertices,instances,first_vertex,first_instance,false); !valid) return valid;
    batch.command().draw(vertices,instances,first_vertex,first_instance);
    return {};
}
Result<void> RenderEncoder::draw_indexed(std::uint32_t indices, std::uint32_t instances, std::uint32_t first_index, std::int32_t vertex_offset, std::uint32_t first_instance)
{
    auto owner = encoder(batch_,generation_,true);
    if (!owner) return std::unexpected(owner.error());
    auto& batch = **owner;
    auto& state = *batch.encoding;
    if (!indices || !instances || !state.index) return std::unexpected(bad_state("indexed draw requires counts and an index buffer"));
    const auto stride = state.index_type == vk::IndexType::eUint16 ? 2u : 4u;
    if (!valid_range(state.index->buffer_desc.size - state.index_offset, vk::DeviceSize{first_index} * stride, vk::DeviceSize{indices} * stride))
        return std::unexpected(invalid("index input range exceeds buffer"));
    if (auto valid = require_use(batch,state.index,vk::PipelineStageFlagBits2::eIndexInput,vk::AccessFlagBits2::eIndexRead); !valid) return valid;
    if (auto valid = validate_bindings(batch); !valid) return valid;
    if (auto valid = validate_vertices(batch,indices,instances,0,first_instance,true); !valid) return valid;
    batch.command().drawIndexed(indices,instances,first_index,vertex_offset,first_instance);
    return {};
}
Result<void> RenderEncoder::end()
{
    auto owner = encoder(batch_,generation_,true);
    if (!owner) return std::unexpected(owner.error());
    auto& batch = **owner;
    const auto finish = [&](const std::shared_ptr<ViewState>& view, vk::AttachmentStoreOp store) {
        consume_use(batch,view->image,&view->description.range);
        if (store == vk::AttachmentStoreOp::eDontCare) {
            const auto& range = view->description.range;
            batch.find(view->image)->states[static_cast<std::size_t>(range.baseArrayLayer)*view->image->image_desc.mip_levels+range.baseMipLevel].initialized = false;
        }
    };
    batch.command().endRendering();
    finish(batch.encoding->color,batch.encoding->color_store);
    if (batch.encoding->depth) finish(batch.encoding->depth,batch.encoding->depth_store);
    batch.rendering = false;
    invalidate_encoder(batch);
    return {};
}
} // namespace dk::graphics

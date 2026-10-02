#include "PipelinePolicy.hpp"
#include <tuple>

namespace dk::graphics::detail {
vk::ShaderStageFlagBits native_stage(ShaderStage stage) noexcept
{
    switch (stage) {
    case ShaderStage::vertex: return vk::ShaderStageFlagBits::eVertex;
    case ShaderStage::fragment: return vk::ShaderStageFlagBits::eFragment;
    case ShaderStage::compute: return vk::ShaderStageFlagBits::eCompute;
    }
    return static_cast<vk::ShaderStageFlagBits>(0);
}
vk::DescriptorType native_descriptor(ShaderDescriptorType type) noexcept
{
    switch (type) {
    case ShaderDescriptorType::uniform_buffer: return vk::DescriptorType::eUniformBuffer;
    case ShaderDescriptorType::storage_buffer: return vk::DescriptorType::eStorageBuffer;
    case ShaderDescriptorType::sampled_image: return vk::DescriptorType::eSampledImage;
    case ShaderDescriptorType::storage_image: return vk::DescriptorType::eStorageImage;
    case ShaderDescriptorType::sampler: return vk::DescriptorType::eSampler;
    }
    return static_cast<vk::DescriptorType>(-1);
}
namespace {
Error invalid(const char* message) { return {ErrorCode::invalid_argument, message}; }
Error unsupported(const char* message) { return {ErrorCode::not_supported, message}; }
bool same_binding(const LayoutBinding& a, const LayoutBinding& b) noexcept
{
    return a.set == b.set && a.binding == b.binding && a.type == b.type && a.count == b.count && a.stages == b.stages;
}
}
bool compatible_layouts(const LayoutState& a, const LayoutState& b, std::uint32_t set) noexcept
{
    if (a.owner != b.owner || set >= a.sets.size() || set >= b.sets.size() || a.push != b.push) return false;
    const auto ae = std::find_if(a.bindings.begin(), a.bindings.end(), [=](const auto& v) { return v.set > set; });
    const auto be = std::find_if(b.bindings.begin(), b.bindings.end(), [=](const auto& v) { return v.set > set; });
    return std::equal(a.bindings.begin(), ae, b.bindings.begin(), be, same_binding);
}
Result<LayoutDescription> merge_interfaces(memory::ResourceHandle resource, std::span<const ShaderLayoutView> shaders,
                                          const vk::PhysicalDeviceLimits& limits)
{
    if (shaders.empty() || shaders.size() > 3) return std::unexpected(invalid("layout requires 1..3 distinct shader stages"));
    LayoutDescription result{resource};
    vk::ShaderStageFlags used{};
    for (const auto& shader : shaders) {
        const auto stage = native_stage(shader.stage);
        if (!static_cast<VkShaderStageFlags>(stage) || (used & stage)) return std::unexpected(invalid("invalid or duplicate layout shader stage"));
        used |= stage;
        for (std::size_t i = 0; i < shader.bindings.size(); ++i) {
            const auto& input = shader.bindings[i];
            if (!input.count || input.set >= limits.maxBoundDescriptorSets ||
                native_descriptor(input.type) == static_cast<vk::DescriptorType>(-1))
                return std::unexpected(invalid("invalid descriptor count/type/set"));
            for (std::size_t j = 0; j < i; ++j)
                if (shader.bindings[j].set == input.set && shader.bindings[j].binding == input.binding)
                    return std::unexpected(invalid("duplicate reflected descriptor binding within a stage"));
            auto found = std::find_if(result.bindings.begin(), result.bindings.end(), [&](const auto& v) {
                return v.set == input.set && v.binding == input.binding;
            });
            if (found == result.bindings.end()) result.bindings.push_back({input.set, input.binding, input.type, input.count, input.block_size, stage});
            else {
                if (found->type != input.type || found->count != input.count)
                    return std::unexpected(invalid("shader descriptor type/count conflicts across stages"));
                found->stages |= stage;
                found->minimum_buffer_size = std::max(found->minimum_buffer_size, input.block_size);
            }
        }
        std::uint32_t first = limits.maxPushConstantsSize, last = 0;
        for (const auto& push : shader.push) {
            if (!push.size || push.offset % 4 || push.size % 4 || push.offset > limits.maxPushConstantsSize ||
                push.size > limits.maxPushConstantsSize - push.offset)
                return std::unexpected(invalid("push constant range exceeds alignment or device limits"));
            first = std::min(first, push.offset);
            last = std::max(last, push.offset + push.size);
        }
        if (last) {
            auto same = std::find_if(result.push.begin(), result.push.end(), [&](const auto& p) { return p.offset == first && p.size == last-first; });
            if (same == result.push.end()) result.push.push_back({stage, first, last-first});
            else same->stageFlags |= stage;
        }
    }
    std::sort(result.bindings.begin(), result.bindings.end(), [](const auto& a, const auto& b) {
        return std::tie(a.set, a.binding) < std::tie(b.set, b.binding);
    });
    std::sort(result.push.begin(), result.push.end(), [](const auto& a, const auto& b) {
        return std::tuple{a.offset, a.size, static_cast<VkShaderStageFlags>(a.stageFlags)} <
            std::tuple{b.offset, b.size, static_cast<VkShaderStageFlags>(b.stageFlags)};
    });
    const std::array<std::uint32_t, 5> total_limits{limits.maxDescriptorSetUniformBuffers, limits.maxDescriptorSetStorageBuffers,
        limits.maxDescriptorSetSampledImages, limits.maxDescriptorSetStorageImages, limits.maxDescriptorSetSamplers};
    const std::array<std::uint32_t, 5> stage_limits{limits.maxPerStageDescriptorUniformBuffers, limits.maxPerStageDescriptorStorageBuffers,
        limits.maxPerStageDescriptorSampledImages, limits.maxPerStageDescriptorStorageImages, limits.maxPerStageDescriptorSamplers};
    std::array<std::uint64_t, 5> totals{};
    std::array<std::array<std::uint64_t, 5>, 3> per_stage{};
    constexpr std::array stages{vk::ShaderStageFlagBits::eVertex, vk::ShaderStageFlagBits::eFragment, vk::ShaderStageFlagBits::eCompute};
    for (const auto& binding : result.bindings) {
        const auto type = static_cast<std::size_t>(binding.type);
        totals[type] += binding.count;
        if (totals[type] > total_limits[type]) return std::unexpected(unsupported("pipeline layout exceeds descriptor limits"));
        for (std::size_t s = 0; s < stages.size(); ++s) if (binding.stages & stages[s]) {
            per_stage[s][type] += binding.count;
            if (per_stage[s][type] > stage_limits[type]) return std::unexpected(unsupported("pipeline layout exceeds per-stage descriptor limits"));
        }
    }
    for (const auto& counts : per_stage) {
        // Standalone samplers do not count toward maxPerStageResources.
        if (counts[0]+counts[1]+counts[2]+counts[3] > limits.maxPerStageResources)
            return std::unexpected(unsupported("pipeline layout exceeds maxPerStageResources"));
    }
    return result;
}
std::uint32_t vertex_format_size(vk::Format format) noexcept
{
    switch (format) {
    case vk::Format::eR32Sfloat: case vk::Format::eR32Uint: case vk::Format::eR32Sint: return 4;
    case vk::Format::eR32G32Sfloat: case vk::Format::eR32G32Uint: case vk::Format::eR32G32Sint: return 8;
    case vk::Format::eR32G32B32Sfloat: case vk::Format::eR32G32B32Uint: case vk::Format::eR32G32B32Sint: return 12;
    case vk::Format::eR32G32B32A32Sfloat: case vk::Format::eR32G32B32A32Uint: case vk::Format::eR32G32B32A32Sint: return 16;
    case vk::Format::eR8G8B8A8Unorm: return 4;
    default: return 0;
    }
}
} // namespace dk::graphics::detail

namespace dk::graphics {
vk::PipelineLayout PipelineLayout::handle() const noexcept { return state_ ? *state_->layout : vk::PipelineLayout{}; }
std::span<const LayoutBinding> PipelineLayout::bindings() const noexcept { return state_ ? std::span<const LayoutBinding>{state_->bindings} : std::span<const LayoutBinding>{}; }
std::span<const vk::PushConstantRange> PipelineLayout::push_constants() const noexcept { return state_ ? std::span<const vk::PushConstantRange>{state_->push} : std::span<const vk::PushConstantRange>{}; }
bool PipelineLayout::compatible_for_set(const PipelineLayout& other, std::uint32_t set) const noexcept
{ return state_ && other.state_ && detail::compatible_layouts(*state_, *other.state_, set); }
vk::Pipeline ComputePipeline::handle() const noexcept { return state_ ? *state_->pipeline : vk::Pipeline{}; }
vk::Pipeline GraphicsPipeline::handle() const noexcept { return state_ ? *state_->pipeline : vk::Pipeline{}; }
std::array<std::uint32_t, 3> ComputePipeline::thread_group_size() const noexcept { return state_ ? state_->group : std::array<std::uint32_t,3>{}; }
Result<PipelineLayout> ResourceFactory::create_pipeline_layout(std::span<const ShaderModule* const> shaders) const
{
    auto checked = detail::factory_queue(queue_);
    if (!checked) return std::unexpected(checked.error());
    auto& queue = **checked;
    Vector<detail::ShaderLayoutView> inputs{memory::Allocator<detail::ShaderLayoutView>{queue.resource}};
    for (const auto* shader : shaders) {
        if (!shader || !shader->state_ || shader->state_->owner != queue.owner)
            return std::unexpected(Error{ErrorCode::invalid_argument, "layout shader is empty or belongs to another device"});
        inputs.push_back({shader->state_->stage, shader->state_->bindings, shader->state_->push});
    }
    auto merged = detail::merge_interfaces(queue.resource, inputs, queue.owner->device.adapter().properties.limits);
    if (!merged) return std::unexpected(merged.error());
    auto state = memory::make_shared_in<detail::LayoutState>(queue.resource, queue.owner, queue.resource);
    state->bindings = std::move(merged->bindings);
    state->push = std::move(merged->push);
    const auto count = state->bindings.empty() ? 0u : state->bindings.back().set + 1;
    state->sets.reserve(count);
    Vector<vk::DescriptorSetLayout> handles{memory::Allocator<vk::DescriptorSetLayout>{queue.resource}};
    handles.reserve(count);
    try {
        for (std::uint32_t index = 0; index < count; ++index) {
            Vector<vk::DescriptorSetLayoutBinding> bindings{memory::Allocator<vk::DescriptorSetLayoutBinding>{queue.resource}};
            for (const auto& binding : state->bindings) if (binding.set == index)
                bindings.emplace_back(binding.binding, detail::native_descriptor(binding.type), binding.count, binding.stages);
            vk::DescriptorSetLayoutCreateInfo info{};
            info.setBindings(bindings);
            state->sets.emplace_back(queue.owner->device.logical_device(), info);
            handles.push_back(*state->sets.back());
        }
        vk::PipelineLayoutCreateInfo info{};
        info.setSetLayouts(handles).setPushConstantRanges(state->push);
        state->layout = vk::raii::PipelineLayout{queue.owner->device.logical_device(), info};
    } catch (const vk::SystemError& error) { return std::unexpected(queue.failure("create pipeline layout", static_cast<VkResult>(error.code().value()))); }
    if (auto status = detail::object_creation_status(queue, "create pipeline layout"); !status) return std::unexpected(status.error());
    return PipelineLayout{std::move(state)};
}
namespace {
Result<void> check_stage(const detail::QueueState& queue, const ShaderModule* shader, const PipelineLayout* layout, ShaderStage expected)
{
    if (!shader || !*shader || !layout || !*layout)
        return std::unexpected(Error{ErrorCode::invalid_argument, "pipeline requires shader and layout"});
    const auto& s = *detail::ObjectAccess::state(*shader);
    const auto& l = *detail::ObjectAccess::state(*layout);
    if (s.owner != queue.owner || l.owner != queue.owner || s.stage != expected)
        return std::unexpected(Error{ErrorCode::invalid_argument, "pipeline shader stage or owner mismatch"});
    const auto stage = detail::native_stage(expected);
    for (const auto& binding : s.bindings) {
        const auto found = std::find_if(l.bindings.begin(), l.bindings.end(), [&](const auto& b) {
            return b.set == binding.set && b.binding == binding.binding && b.type == binding.type &&
                b.count >= binding.count && (b.stages & stage) && b.minimum_buffer_size >= binding.block_size;
        });
        if (found == l.bindings.end()) return std::unexpected(Error{ErrorCode::invalid_argument, "pipeline layout does not cover shader descriptors"});
    }
    for (const auto& push : s.push) {
        const auto found = std::find_if(l.push.begin(), l.push.end(), [&](const auto& p) {
            return (p.stageFlags & stage) && push.offset >= p.offset && push.offset - p.offset <= p.size &&
                push.size <= p.size - (push.offset - p.offset);
        });
        if (found == l.push.end()) return std::unexpected(Error{ErrorCode::invalid_argument, "pipeline layout does not cover shader push constants"});
    }
    return {};
}
}
Result<ComputePipeline> ResourceFactory::create_compute_pipeline(const ComputePipelineDesc& desc) const
{
    auto checked = detail::factory_queue(queue_);
    if (!checked) return std::unexpected(checked.error());
    auto& queue = **checked;
    if (auto valid = check_stage(queue, desc.shader, desc.layout, ShaderStage::compute); !valid) return std::unexpected(valid.error());
    const auto& shader = *desc.shader->state_;
    const auto& limits = queue.owner->device.adapter().properties.limits;
    std::uint32_t total = 1;
    for (std::size_t i = 0; i < 3; ++i) {
        if (!shader.group[i] || shader.group[i] > limits.maxComputeWorkGroupSize[i] || shader.group[i] > limits.maxComputeWorkGroupInvocations / total)
            return std::unexpected(Error{ErrorCode::not_supported, "compute local workgroup exceeds device limits"});
        total *= shader.group[i];
    }
    auto state = memory::make_shared_in<detail::PipelineState>(queue.resource, queue.owner, queue.resource, desc.layout->state_);
    state->group = shader.group;
    const vk::PipelineShaderStageCreateInfo stage{{}, vk::ShaderStageFlagBits::eCompute, *shader.shader, shader.entry.c_str()};
    try { state->pipeline = vk::raii::Pipeline{queue.owner->device.logical_device(), nullptr, vk::ComputePipelineCreateInfo{{}, stage, desc.layout->handle()}}; }
    catch (const vk::SystemError& error) { return std::unexpected(queue.failure("create compute pipeline", static_cast<VkResult>(error.code().value()))); }
    if (auto status = detail::object_creation_status(queue, "create compute pipeline"); !status) return std::unexpected(status.error());
    return ComputePipeline{std::move(state)};
}
Result<GraphicsPipeline> ResourceFactory::create_graphics_pipeline(const GraphicsPipelineDesc& desc) const
{
    auto checked = detail::factory_queue(queue_);
    if (!checked) return std::unexpected(checked.error());
    auto& queue = **checked;
    if (auto valid = check_stage(queue, desc.vertex, desc.layout, ShaderStage::vertex); !valid) return std::unexpected(valid.error());
    if (auto valid = check_stage(queue, desc.fragment, desc.layout, ShaderStage::fragment); !valid) return std::unexpected(valid.error());
    const auto& device = queue.owner->device;
    const auto& limits = device.adapter().properties.limits;
    if (desc.color_format != vk::Format::eR8G8B8A8Unorm && desc.color_format != vk::Format::eR8G8B8A8Srgb &&
        desc.color_format != vk::Format::eB8G8R8A8Unorm && desc.color_format != vk::Format::eB8G8R8A8Srgb &&
        desc.color_format != vk::Format::eR32Uint && desc.color_format != vk::Format::eR32Sfloat)
        return std::unexpected(Error{ErrorCode::not_supported, "color format is outside supported resource formats"});
    if ((desc.topology != vk::PrimitiveTopology::eTriangleList && desc.topology != vk::PrimitiveTopology::eTriangleStrip) ||
        (desc.front_face != vk::FrontFace::eClockwise && desc.front_face != vk::FrontFace::eCounterClockwise) ||
        (static_cast<VkCullModeFlags>(desc.cull_mode) & ~static_cast<VkCullModeFlags>(vk::CullModeFlagBits::eFrontAndBack)) ||
        (desc.depth_format != vk::Format::eUndefined && desc.depth_format != vk::Format::eD32Sfloat) ||
        ((desc.depth_test || desc.depth_write) && desc.depth_format == vk::Format::eUndefined) ||
        static_cast<std::uint32_t>(desc.depth_compare) > static_cast<std::uint32_t>(vk::CompareOp::eAlways))
        return std::unexpected(Error{ErrorCode::not_supported, "unsupported graphics topology/raster/depth state"});
    const auto color = device.physical_device().getFormatProperties(desc.color_format).optimalTilingFeatures;
    if (!(color & vk::FormatFeatureFlagBits::eColorAttachment) || (desc.blend && !(color & vk::FormatFeatureFlagBits::eColorAttachmentBlend)))
        return std::unexpected(Error{ErrorCode::not_supported, "color format lacks attachment/blend support"});
    if (desc.depth_format != vk::Format::eUndefined && !(device.physical_device().getFormatProperties(desc.depth_format).optimalTilingFeatures & vk::FormatFeatureFlagBits::eDepthStencilAttachment))
        return std::unexpected(Error{ErrorCode::not_supported, "depth format lacks attachment support"});
    if (desc.vertex_bindings.size() > limits.maxVertexInputBindings || desc.vertex_attributes.size() > limits.maxVertexInputAttributes)
        return std::unexpected(Error{ErrorCode::not_supported, "vertex input exceeds device limits"});
    for (std::size_t i = 0; i < desc.vertex_bindings.size(); ++i) {
        const auto& b = desc.vertex_bindings[i];
        if (b.binding >= limits.maxVertexInputBindings || b.stride > limits.maxVertexInputBindingStride ||
            (b.inputRate != vk::VertexInputRate::eVertex && b.inputRate != vk::VertexInputRate::eInstance))
            return std::unexpected(Error{ErrorCode::invalid_argument, "invalid vertex binding"});
        for (std::size_t j = 0; j < i; ++j) if (desc.vertex_bindings[j].binding == b.binding)
            return std::unexpected(Error{ErrorCode::invalid_argument, "duplicate vertex binding"});
    }
    for (std::size_t i = 0; i < desc.vertex_attributes.size(); ++i) {
        const auto& a = desc.vertex_attributes[i];
        if (!detail::vertex_format_size(a.format) || a.location >= limits.maxVertexInputAttributes || a.offset > limits.maxVertexInputAttributeOffset ||
            std::none_of(desc.vertex_bindings.begin(), desc.vertex_bindings.end(), [&](const auto& b) { return b.binding == a.binding; }) ||
            !(device.physical_device().getFormatProperties(a.format).bufferFeatures & vk::FormatFeatureFlagBits::eVertexBuffer))
            return std::unexpected(Error{ErrorCode::invalid_argument, "invalid vertex attribute format/location/binding/offset"});
        for (std::size_t j = 0; j < i; ++j) if (desc.vertex_attributes[j].location == a.location)
            return std::unexpected(Error{ErrorCode::invalid_argument, "duplicate vertex location"});
    }
    auto state = memory::make_shared_in<detail::PipelineState>(queue.resource, queue.owner, queue.resource, desc.layout->state_);
    state->point = vk::PipelineBindPoint::eGraphics;
    state->color_format = desc.color_format;
    state->depth_format = desc.depth_format;
    state->topology = desc.topology;
    state->vertex_bindings.assign(desc.vertex_bindings.begin(), desc.vertex_bindings.end());
    state->vertex_attributes.assign(desc.vertex_attributes.begin(), desc.vertex_attributes.end());
    const std::array stages{
        vk::PipelineShaderStageCreateInfo{{}, vk::ShaderStageFlagBits::eVertex, desc.vertex->handle(), desc.vertex->state_->entry.c_str()},
        vk::PipelineShaderStageCreateInfo{{}, vk::ShaderStageFlagBits::eFragment, desc.fragment->handle(), desc.fragment->state_->entry.c_str()}};
    vk::PipelineVertexInputStateCreateInfo input{};
    input.setVertexBindingDescriptions(desc.vertex_bindings).setVertexAttributeDescriptions(desc.vertex_attributes);
    const vk::PipelineInputAssemblyStateCreateInfo assembly{{}, desc.topology, VK_FALSE};
    vk::PipelineViewportStateCreateInfo viewport{};
    viewport.viewportCount = viewport.scissorCount = 1;
    vk::PipelineRasterizationStateCreateInfo raster{};
    raster.setPolygonMode(vk::PolygonMode::eFill).setCullMode(desc.cull_mode).setFrontFace(desc.front_face).setLineWidth(1);
    const vk::PipelineMultisampleStateCreateInfo samples{{}, vk::SampleCountFlagBits::e1};
    vk::PipelineDepthStencilStateCreateInfo depth{};
    depth.setDepthTestEnable(desc.depth_test).setDepthWriteEnable(desc.depth_write).setDepthCompareOp(desc.depth_compare);
    vk::PipelineColorBlendAttachmentState attachment{};
    attachment.setBlendEnable(desc.blend).setSrcColorBlendFactor(vk::BlendFactor::eSrcAlpha).setDstColorBlendFactor(vk::BlendFactor::eOneMinusSrcAlpha)
        .setColorBlendOp(vk::BlendOp::eAdd).setSrcAlphaBlendFactor(vk::BlendFactor::eOne).setDstAlphaBlendFactor(vk::BlendFactor::eOneMinusSrcAlpha)
        .setAlphaBlendOp(vk::BlendOp::eAdd).setColorWriteMask(vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG | vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA);
    vk::PipelineColorBlendStateCreateInfo blend{};
    blend.setAttachments(attachment);
    const std::array dynamic_states{vk::DynamicState::eViewport, vk::DynamicState::eScissor};
    vk::PipelineDynamicStateCreateInfo dynamic{};
    dynamic.setDynamicStates(dynamic_states);
    vk::PipelineRenderingCreateInfo rendering{};
    rendering.setColorAttachmentFormats(desc.color_format).setDepthAttachmentFormat(desc.depth_format);
    vk::GraphicsPipelineCreateInfo info{};
    info.setStages(stages).setPVertexInputState(&input).setPInputAssemblyState(&assembly).setPViewportState(&viewport)
        .setPRasterizationState(&raster).setPMultisampleState(&samples).setPDepthStencilState(&depth)
        .setPColorBlendState(&blend).setPDynamicState(&dynamic).setLayout(desc.layout->handle()).setPNext(&rendering);
    try { state->pipeline = vk::raii::Pipeline{device.logical_device(), nullptr, info}; }
    catch (const vk::SystemError& error) { return std::unexpected(queue.failure("create graphics pipeline", static_cast<VkResult>(error.code().value()))); }
    if (auto status = detail::object_creation_status(queue, "create graphics pipeline"); !status) return std::unexpected(status.error());
    return GraphicsPipeline{std::move(state)};
}
} // namespace dk::graphics

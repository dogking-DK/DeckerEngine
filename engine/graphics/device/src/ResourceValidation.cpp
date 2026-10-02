#include <dk/graphics/ResourceValidation.hpp>
#include "ResourcePolicy.hpp"
#include "AccessMasks.hpp"

namespace dk::graphics {
using namespace detail;
namespace {
Result<void> validate_access(const AccessDescription& input)
{
    const auto& state = input.state;
    constexpr auto allowed_stages = vk::PipelineStageFlagBits2::eTopOfPipe | vk::PipelineStageFlagBits2::eBottomOfPipe |
        vk::PipelineStageFlagBits2::eAllCommands | vk::PipelineStageFlagBits2::eAllGraphics | vk::PipelineStageFlagBits2::eAllTransfer |
        vk::PipelineStageFlagBits2::eCopy | vk::PipelineStageFlagBits2::eBlit | vk::PipelineStageFlagBits2::eClear |
        vk::PipelineStageFlagBits2::eVertexInput | vk::PipelineStageFlagBits2::eIndexInput | vk::PipelineStageFlagBits2::eVertexAttributeInput |
        vk::PipelineStageFlagBits2::eVertexShader | vk::PipelineStageFlagBits2::eFragmentShader | vk::PipelineStageFlagBits2::eComputeShader |
        vk::PipelineStageFlagBits2::eEarlyFragmentTests | vk::PipelineStageFlagBits2::eLateFragmentTests |
        vk::PipelineStageFlagBits2::eColorAttachmentOutput | vk::PipelineStageFlagBits2::eHost;
    if (!state.stages || !state.access || (static_cast<VkPipelineStageFlags2>(state.stages) & ~static_cast<VkPipelineStageFlags2>(allowed_stages)) ||
        (static_cast<VkAccessFlags2>(state.access) & ~static_cast<VkAccessFlags2>(read_access | write_access)) ||
        (input.full_overwrite && !(state.access & write_access)))
        return std::unexpected(Error{ErrorCode::invalid_argument, "unsupported resource access/stages or overwrite declaration"});
    const auto has_stage = [&](vk::PipelineStageFlags2 choices) {
        constexpr auto graphics = vk::PipelineStageFlagBits2::eVertexShader | vk::PipelineStageFlagBits2::eFragmentShader |
            vk::PipelineStageFlagBits2::eVertexInput | vk::PipelineStageFlagBits2::eIndexInput | vk::PipelineStageFlagBits2::eVertexAttributeInput |
            vk::PipelineStageFlagBits2::eEarlyFragmentTests | vk::PipelineStageFlagBits2::eLateFragmentTests | vk::PipelineStageFlagBits2::eColorAttachmentOutput;
        return bool(state.stages & choices) || bool(state.stages & vk::PipelineStageFlagBits2::eAllCommands) ||
            (bool(state.stages & vk::PipelineStageFlagBits2::eAllGraphics) && bool(choices & graphics)) ||
            (bool(state.stages & vk::PipelineStageFlagBits2::eVertexInput) && bool(choices & (vk::PipelineStageFlagBits2::eIndexInput | vk::PipelineStageFlagBits2::eVertexAttributeInput)));
    };
    if ((state.access & (vk::AccessFlagBits2::eTransferRead | vk::AccessFlagBits2::eTransferWrite)) &&
        !has_stage(vk::PipelineStageFlagBits2::eAllTransfer | vk::PipelineStageFlagBits2::eCopy | vk::PipelineStageFlagBits2::eBlit | vk::PipelineStageFlagBits2::eClear))
        return std::unexpected(Error{ErrorCode::invalid_argument, "transfer access requires transfer stages"});
    if ((state.access & (vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite | vk::AccessFlagBits2::eShaderSampledRead |
        vk::AccessFlagBits2::eShaderStorageRead | vk::AccessFlagBits2::eShaderStorageWrite | vk::AccessFlagBits2::eUniformRead)) &&
        !has_stage(vk::PipelineStageFlagBits2::eVertexShader | vk::PipelineStageFlagBits2::eFragmentShader | vk::PipelineStageFlagBits2::eComputeShader))
        return std::unexpected(Error{ErrorCode::invalid_argument, "shader access requires shader stages"});
    const auto stage_matches = [&](vk::AccessFlags2 access, vk::PipelineStageFlags2 stages) { return !(state.access & access) || has_stage(stages); };
    if (((state.access & (vk::AccessFlagBits2::eHostRead | vk::AccessFlagBits2::eHostWrite)) && !(state.stages & vk::PipelineStageFlagBits2::eHost)) ||
        !stage_matches(vk::AccessFlagBits2::eIndexRead, vk::PipelineStageFlagBits2::eIndexInput) ||
        !stage_matches(vk::AccessFlagBits2::eVertexAttributeRead, vk::PipelineStageFlagBits2::eVertexAttributeInput) ||
        !stage_matches(vk::AccessFlagBits2::eColorAttachmentRead | vk::AccessFlagBits2::eColorAttachmentWrite, vk::PipelineStageFlagBits2::eColorAttachmentOutput) ||
        !stage_matches(vk::AccessFlagBits2::eDepthStencilAttachmentRead | vk::AccessFlagBits2::eDepthStencilAttachmentWrite, vk::PipelineStageFlagBits2::eEarlyFragmentTests | vk::PipelineStageFlagBits2::eLateFragmentTests))
        return std::unexpected(Error{ErrorCode::invalid_argument, "access and pipeline stages are incompatible"});
    return {};
}
} // namespace
Result<void> validate_buffer_description(const BufferDesc& desc) { return detail::validate_buffer(desc); }
Result<void> validate_image_description(const ImageDesc& desc) { return detail::validate_image(desc); }
bool access_reads(vk::AccessFlags2 access) noexcept { return bool(access & read_access); }
bool access_writes(vk::AccessFlags2 access) noexcept { return bool(access & write_access); }
Result<AccessDescription> validate_buffer_access(const BufferDesc& desc, const AccessDescription& input)
{
    if (auto valid = validate_buffer_description(desc); !valid) return std::unexpected(valid.error());
    if (auto valid = validate_access(input); !valid) return std::unexpected(valid.error());
    const auto& state = input.state;
    auto use = input;
    if (((state.access & vk::AccessFlagBits2::eHostRead) && desc.memory != BufferMemory::readback) ||
        ((state.access & vk::AccessFlagBits2::eHostWrite) && desc.memory != BufferMemory::upload))
        return std::unexpected(Error{ErrorCode::invalid_argument, "host access does not match buffer memory role"});
    if (state.access & (vk::AccessFlagBits2::eShaderSampledRead | vk::AccessFlagBits2::eColorAttachmentRead | vk::AccessFlagBits2::eColorAttachmentWrite |
        vk::AccessFlagBits2::eDepthStencilAttachmentRead | vk::AccessFlagBits2::eDepthStencilAttachmentWrite))
        return std::unexpected(Error{ErrorCode::invalid_argument, "image access requested for buffer"});
    if (state.layout != vk::ImageLayout::eUndefined || input.offset > desc.size)
        return std::unexpected(Error{ErrorCode::invalid_argument, "buffer use layout/offset invalid"});
    use.size = input.size == VK_WHOLE_SIZE ? desc.size-input.offset : input.size;
    if (!use.size || !valid_range(desc.size, use.offset, use.size)) return std::unexpected(Error{ErrorCode::invalid_argument, "buffer use range invalid"});
    const auto usage_for = [&](vk::AccessFlags2 access, vk::BufferUsageFlags usage) { return !(state.access & access) || bool(desc.usage & usage); };
    if (!usage_for(vk::AccessFlagBits2::eTransferRead, vk::BufferUsageFlagBits::eTransferSrc) ||
        !usage_for(vk::AccessFlagBits2::eTransferWrite, vk::BufferUsageFlagBits::eTransferDst) ||
        !usage_for(vk::AccessFlagBits2::eUniformRead, vk::BufferUsageFlagBits::eUniformBuffer) ||
        !usage_for(vk::AccessFlagBits2::eVertexAttributeRead, vk::BufferUsageFlagBits::eVertexBuffer) ||
        !usage_for(vk::AccessFlagBits2::eIndexRead, vk::BufferUsageFlagBits::eIndexBuffer) ||
        !usage_for(vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite | vk::AccessFlagBits2::eShaderStorageRead | vk::AccessFlagBits2::eShaderStorageWrite, vk::BufferUsageFlagBits::eStorageBuffer))
        return std::unexpected(Error{ErrorCode::invalid_argument, "buffer access does not match usage"});
    return use;
}
Result<AccessDescription> validate_image_access(const ImageDesc& desc, const AccessDescription& input)
{
    if (auto valid = validate_image_description(desc); !valid) return std::unexpected(valid.error());
    if (auto valid = validate_access(input); !valid) return std::unexpected(valid.error());
    const auto& state = input.state;
    auto use = input;
    if (state.access & (vk::AccessFlagBits2::eHostRead | vk::AccessFlagBits2::eHostWrite | vk::AccessFlagBits2::eIndexRead |
        vk::AccessFlagBits2::eVertexAttributeRead | vk::AccessFlagBits2::eUniformRead))
        return std::unexpected(Error{ErrorCode::invalid_argument, "buffer access requested for image"});
    if (auto valid = validate_subresources(desc, use.range); !valid) return std::unexpected(valid.error());
    if (auto valid = validate_layout(state.layout, desc.usage); !valid) return std::unexpected(valid.error());
    vk::AccessFlags2 layout_access = read_access | write_access;
    switch (state.layout) {
    case vk::ImageLayout::eTransferSrcOptimal: layout_access = vk::AccessFlagBits2::eTransferRead; break;
    case vk::ImageLayout::eTransferDstOptimal: layout_access = vk::AccessFlagBits2::eTransferWrite; break;
    case vk::ImageLayout::eShaderReadOnlyOptimal: layout_access = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderSampledRead; break;
    case vk::ImageLayout::eColorAttachmentOptimal: layout_access = vk::AccessFlagBits2::eColorAttachmentRead | vk::AccessFlagBits2::eColorAttachmentWrite; break;
    case vk::ImageLayout::eDepthStencilAttachmentOptimal: layout_access = vk::AccessFlagBits2::eDepthStencilAttachmentRead | vk::AccessFlagBits2::eDepthStencilAttachmentWrite; break;
    default: break;
    }
    if (state.access & ~layout_access) return std::unexpected(Error{ErrorCode::invalid_argument, "image access incompatible with layout"});
    const auto usage_for = [&](vk::AccessFlags2 access, vk::ImageUsageFlags usage) { return !(state.access & access) || bool(desc.usage & usage); };
    if (!usage_for(vk::AccessFlagBits2::eTransferRead, vk::ImageUsageFlagBits::eTransferSrc) ||
        !usage_for(vk::AccessFlagBits2::eTransferWrite, vk::ImageUsageFlagBits::eTransferDst) ||
        !usage_for(vk::AccessFlagBits2::eShaderSampledRead, vk::ImageUsageFlagBits::eSampled) ||
        !usage_for(vk::AccessFlagBits2::eShaderWrite | vk::AccessFlagBits2::eShaderStorageRead | vk::AccessFlagBits2::eShaderStorageWrite, vk::ImageUsageFlagBits::eStorage) ||
        !usage_for(vk::AccessFlagBits2::eColorAttachmentRead | vk::AccessFlagBits2::eColorAttachmentWrite, vk::ImageUsageFlagBits::eColorAttachment) ||
        !usage_for(vk::AccessFlagBits2::eDepthStencilAttachmentRead | vk::AccessFlagBits2::eDepthStencilAttachmentWrite, vk::ImageUsageFlagBits::eDepthStencilAttachment))
        return std::unexpected(Error{ErrorCode::invalid_argument, "image access does not match usage"});
    return use;
}
} // namespace dk::graphics

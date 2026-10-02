#include "ObjectInternal.hpp"
#include "ResourcePolicy.hpp"
#include <cmath>

namespace dk::graphics {
ResourceFactory SubmissionQueue::resources() const noexcept { return ResourceFactory{state_}; }
vk::ImageView ImageView::handle() const noexcept { return state_ ? *state_->view : vk::ImageView{}; }
ImageViewDesc ImageView::description() const noexcept { return state_ ? state_->description : ImageViewDesc{}; }
vk::Sampler Sampler::handle() const noexcept { return state_ ? *state_->sampler : vk::Sampler{}; }
vk::ShaderModule ShaderModule::handle() const noexcept { return state_ ? *state_->shader : vk::ShaderModule{}; }
ShaderStage ShaderModule::stage() const noexcept { return state_ ? state_->stage : ShaderStage::compute; }
std::string_view ShaderModule::entry() const noexcept { return state_ ? std::string_view{state_->entry} : std::string_view{}; }
Result<Buffer> ResourceFactory::create_buffer(const BufferDesc& description) const
{
    auto queue = detail::factory_queue(queue_);
    if (!queue) return std::unexpected(queue.error());
    return SubmissionQueue{*queue}.create_buffer(description);
}
Result<Image> ResourceFactory::create_image(const ImageDesc& description) const
{
    auto queue = detail::factory_queue(queue_);
    if (!queue) return std::unexpected(queue.error());
    return SubmissionQueue{*queue}.create_image(description);
}
Result<ImageView> ResourceFactory::create_view(const Image& image, const ImageViewDesc& description) const
{
    auto checked = detail::factory_queue(queue_);
    if (!checked) return std::unexpected(checked.error());
    auto& queue = **checked;
    if (!image.state_ || image.state_->owner != queue.owner)
        return std::unexpected(Error{ErrorCode::invalid_argument, "view image is empty or belongs to another device"});
    const auto& range = description.range;
    if (auto valid = detail::validate_subresources(image.description(), range); !valid) return std::unexpected(valid.error());
    if ((description.type != vk::ImageViewType::e2D && description.type != vk::ImageViewType::e2DArray) ||
        (description.type == vk::ImageViewType::e2D && range.layerCount != 1))
        return std::unexpected(Error{ErrorCode::not_supported, "view requires 2D or 2D array matching its range"});
    constexpr auto view_usage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eStorage |
        vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eInputAttachment | vk::ImageUsageFlagBits::eDepthStencilAttachment;
    if (!(image.description().usage & view_usage))
        return std::unexpected(Error{ErrorCode::invalid_argument, "image lacks a view-compatible usage"});
    auto state = memory::make_shared_in<detail::ViewState>(queue.resource, queue.owner, image.state_, description);
    try {
        state->view = vk::raii::ImageView{queue.owner->device.logical_device(),
            vk::ImageViewCreateInfo{{}, image.handle(), description.type, image.description().format, {}, range}};
    } catch (const vk::SystemError& error) {
        return std::unexpected(queue.failure("create image view", static_cast<VkResult>(error.code().value())));
    }
    if (auto status = detail::object_creation_status(queue, "create image view"); !status) return std::unexpected(status.error());
    return ImageView{std::move(state)};
}
Result<Sampler> ResourceFactory::create_sampler(const SamplerDesc& desc) const
{
    auto checked = detail::factory_queue(queue_);
    if (!checked) return std::unexpected(checked.error());
    const auto filter = [](vk::Filter value) { return value == vk::Filter::eNearest || value == vk::Filter::eLinear; };
    const auto address = [](vk::SamplerAddressMode value) {
        return value == vk::SamplerAddressMode::eRepeat || value == vk::SamplerAddressMode::eMirroredRepeat ||
            value == vk::SamplerAddressMode::eClampToEdge || value == vk::SamplerAddressMode::eClampToBorder;
    };
    if (!filter(desc.min_filter) || !filter(desc.mag_filter) || !address(desc.address_u) || !address(desc.address_v) ||
        !address(desc.address_w) || (desc.mipmap_mode != vk::SamplerMipmapMode::eNearest && desc.mipmap_mode != vk::SamplerMipmapMode::eLinear) ||
        !std::isfinite(desc.min_lod) || !std::isfinite(desc.max_lod) || desc.min_lod < 0 || desc.min_lod > desc.max_lod)
        return std::unexpected(Error{ErrorCode::invalid_argument, "invalid baseline sampler filter/address/LOD"});
    auto& queue = **checked;
    auto state = memory::make_shared_in<detail::SamplerState>(queue.resource, queue.owner);
    vk::SamplerCreateInfo info{};
    info.setMinFilter(desc.min_filter).setMagFilter(desc.mag_filter).setMipmapMode(desc.mipmap_mode)
        .setAddressModeU(desc.address_u).setAddressModeV(desc.address_v).setAddressModeW(desc.address_w)
        .setMinLod(desc.min_lod).setMaxLod(desc.max_lod);
    try { state->sampler = vk::raii::Sampler{queue.owner->device.logical_device(), info}; }
    catch (const vk::SystemError& error) { return std::unexpected(queue.failure("create sampler", static_cast<VkResult>(error.code().value()))); }
    if (auto status = detail::object_creation_status(queue, "create sampler"); !status) return std::unexpected(status.error());
    return Sampler{std::move(state)};
}
Result<ShaderModule> ResourceFactory::create_shader(const CompiledShader& shader) const
{
    auto checked = detail::factory_queue(queue_);
    if (!checked) return std::unexpected(checked.error());
    if (auto valid = validate_shader_artifact(shader); !valid) return std::unexpected(valid.error());
    auto& queue = **checked;
    auto state = memory::make_shared_in<detail::ShaderState>(queue.resource, queue.owner, queue.resource);
    state->entry = shader.entry;
    state->stage = shader.stage;
    state->group = shader.thread_group_size;
    for (const auto& binding : shader.bindings)
        state->bindings.push_back({binding.set, binding.binding, binding.type, binding.count, binding.block_size});
    state->push.assign(shader.push_constants.begin(), shader.push_constants.end());
    vk::ShaderModuleCreateInfo info{};
    info.setCode(shader.spirv);
    try { state->shader = vk::raii::ShaderModule{queue.owner->device.logical_device(), info}; }
    catch (const vk::SystemError& error) { return std::unexpected(queue.failure("create shader module", static_cast<VkResult>(error.code().value()))); }
    if (auto status = detail::object_creation_status(queue, "create shader module"); !status) return std::unexpected(status.error());
    return ShaderModule{std::move(state)};
}
} // namespace dk::graphics

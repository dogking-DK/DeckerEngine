#include "ObjectInternal.hpp"
#include "ResourcePolicy.hpp"

namespace dk::graphics {
vk::DescriptorSet BindingSet::handle() const noexcept { return state_ ? *state_->set : vk::DescriptorSet{}; }
std::uint32_t BindingSet::set_index() const noexcept { return state_ ? state_->set_index : 0; }
namespace {
constexpr std::uint32_t sets_per_page = 32, max_pages = 64, max_set_descriptors = 4096;
Result<std::shared_ptr<detail::PoolPage>> pool_page(detail::QueueState& queue, const std::array<std::uint32_t,5>& counts)
{
    for (auto& weak : queue.descriptor_pages) if (auto page = weak.lock()) {
        if (page->counts == counts && page->allocations < sets_per_page) return page;
    }
    auto free = std::find_if(queue.descriptor_pages.begin(), queue.descriptor_pages.end(), [](const auto& weak) { return weak.expired(); });
    if (free == queue.descriptor_pages.end() && queue.descriptor_pages.size() >= max_pages)
        return std::unexpected(Error{ErrorCode::conflict, "descriptor pool page capacity exhausted; release completed bindings"});
    // Allocate the weak directory entry before publishing a Vulkan pool.
    if (free == queue.descriptor_pages.end()) {
        queue.descriptor_pages.emplace_back();
        free = queue.descriptor_pages.end() - 1;
    }
    auto page = memory::make_shared_in<detail::PoolPage>(queue.resource, queue.owner);
    page->counts = counts;
    std::array<vk::DescriptorPoolSize, 5> sizes{};
    std::size_t size_count = 0;
    for (std::size_t i = 0; i < counts.size(); ++i) if (counts[i])
        sizes[size_count++] = {detail::native_descriptor(static_cast<ShaderDescriptorType>(i)), counts[i] * sets_per_page};
    vk::DescriptorPoolCreateInfo info{};
    const std::span<const vk::DescriptorPoolSize> page_sizes{sizes.data(), size_count};
    info.setFlags(vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet).setMaxSets(sets_per_page)
        .setPoolSizes(page_sizes);
    page->pool = vk::raii::DescriptorPool{queue.owner->device.logical_device(), info};
    *free = page;
    return page;
}
}
Result<BindingSet> ResourceFactory::create_bindings(const PipelineLayout& layout, std::uint32_t set_index,
                                                  std::span<const BindingWrite> writes) const
{
    auto checked = detail::factory_queue(queue_);
    if (!checked) return std::unexpected(checked.error());
    auto& queue = **checked;
    if (!layout.state_ || layout.state_->owner != queue.owner || set_index >= layout.state_->sets.size())
        return std::unexpected(Error{ErrorCode::invalid_argument, "binding layout/set is empty, out of range or foreign"});
    std::array<std::uint32_t, 5> counts{};
    std::uint64_t expected = 0;
    for (const auto& binding : layout.state_->bindings) if (binding.set == set_index) {
        expected += binding.count;
        if (expected > max_set_descriptors) return std::unexpected(Error{ErrorCode::not_supported, "binding set exceeds 4096 descriptor budget"});
        counts[static_cast<std::size_t>(binding.type)] += binding.count;
    }
    if (writes.size() != expected)
        return std::unexpected(Error{ErrorCode::invalid_argument, "binding set requires every descriptor array element exactly once"});
    auto state = memory::make_shared_in<detail::BindingState>(queue.resource, queue.owner, queue.resource);
    state->layout = layout.state_;
    state->set_index = set_index;
    state->resources.reserve(writes.size());
    const auto& limits = queue.owner->device.adapter().properties.limits;
    for (const auto& write : writes) {
        const auto found = std::find_if(layout.state_->bindings.begin(), layout.state_->bindings.end(), [&](const auto& b) {
            return b.set == set_index && b.binding == write.binding;
        });
        if (found == layout.state_->bindings.end() || write.array_element >= found->count ||
            std::any_of(state->resources.begin(), state->resources.end(), [&](const auto& b) { return b.binding.binding == write.binding && b.element == write.array_element; }))
            return std::unexpected(Error{ErrorCode::invalid_argument, "unknown, duplicate or out-of-range descriptor element"});
        detail::BoundResource bound{};
        bound.binding = *found;
        bound.element = write.array_element;
        if (const auto* b = std::get_if<BufferBinding>(&write.resource)) {
            if (!b->buffer || !b->buffer->state_ || b->buffer->state_->owner != queue.owner ||
                (found->type != ShaderDescriptorType::uniform_buffer && found->type != ShaderDescriptorType::storage_buffer))
                return std::unexpected(Error{ErrorCode::invalid_argument, "buffer descriptor owner/type mismatch"});
            bound.buffer = b->buffer->state_;
            const bool uniform = found->type == ShaderDescriptorType::uniform_buffer;
            const auto usage = uniform ? vk::BufferUsageFlagBits::eUniformBuffer : vk::BufferUsageFlagBits::eStorageBuffer;
            const auto alignment = uniform ? limits.minUniformBufferOffsetAlignment : limits.minStorageBufferOffsetAlignment;
            const auto maximum = uniform ? limits.maxUniformBufferRange : limits.maxStorageBufferRange;
            if (b->offset > b->buffer->size()) return std::unexpected(Error{ErrorCode::invalid_argument, "descriptor buffer offset out of range"});
            bound.offset = b->offset;
            bound.size = b->size == VK_WHOLE_SIZE ? b->buffer->size()-b->offset : b->size;
            if (!bound.size || bound.size < found->minimum_buffer_size || bound.size > maximum ||
                (alignment && b->offset % alignment) || !detail::valid_range(b->buffer->size(), b->offset, bound.size) ||
                !(bound.buffer->buffer_desc.usage & usage))
                return std::unexpected(Error{ErrorCode::invalid_argument, "descriptor buffer range/alignment/usage mismatch"});
        } else if (const auto* image = std::get_if<ImageBinding>(&write.resource)) {
            if (!image->view || !image->view->state_ || image->view->state_->owner != queue.owner ||
                (found->type != ShaderDescriptorType::sampled_image && found->type != ShaderDescriptorType::storage_image))
                return std::unexpected(Error{ErrorCode::invalid_argument, "image descriptor owner/type mismatch"});
            bound.view = image->view->state_;
            bound.image_layout = image->layout;
            const bool storage = found->type == ShaderDescriptorType::storage_image;
            if (!(bound.view->image->image_desc.usage & (storage ? vk::ImageUsageFlagBits::eStorage : vk::ImageUsageFlagBits::eSampled)) ||
                (storage && image->layout != vk::ImageLayout::eGeneral) ||
                (!storage && image->layout != vk::ImageLayout::eGeneral && image->layout != vk::ImageLayout::eShaderReadOnlyOptimal))
                return std::unexpected(Error{ErrorCode::invalid_argument, "descriptor image usage/layout mismatch"});
        } else {
            const auto& sampler = std::get<SamplerBinding>(write.resource);
            if (!sampler.sampler || !sampler.sampler->state_ || sampler.sampler->state_->owner != queue.owner || found->type != ShaderDescriptorType::sampler)
                return std::unexpected(Error{ErrorCode::invalid_argument, "sampler descriptor owner/type mismatch"});
            bound.sampler = sampler.sampler->state_;
        }
        state->resources.push_back(std::move(bound));
    }
    // Stable storage for all Vulkan pointers; no engine allocations after set allocation.
    Vector<vk::DescriptorBufferInfo> buffers{memory::Allocator<vk::DescriptorBufferInfo>{queue.resource}};
    Vector<vk::DescriptorImageInfo> images{memory::Allocator<vk::DescriptorImageInfo>{queue.resource}};
    Vector<vk::WriteDescriptorSet> native_writes{memory::Allocator<vk::WriteDescriptorSet>{queue.resource}};
    buffers.resize(writes.size());
    images.resize(writes.size());
    native_writes.resize(writes.size());
    for (std::size_t i = 0; i < state->resources.size(); ++i) {
        const auto& bound = state->resources[i];
        auto& write = native_writes[i];
        write.setDstBinding(bound.binding.binding).setDstArrayElement(bound.element).setDescriptorCount(1)
            .setDescriptorType(detail::native_descriptor(bound.binding.type));
        if (bound.buffer) {
            buffers[i] = vk::DescriptorBufferInfo{vk::Buffer{bound.buffer->buffer}, bound.offset, bound.size};
            write.setBufferInfo(buffers[i]);
        } else {
            if (bound.view) images[i].setImageView(*bound.view->view).setImageLayout(bound.image_layout);
            if (bound.sampler) images[i].setSampler(*bound.sampler->sampler);
            write.setImageInfo(images[i]);
        }
    }
    try {
        auto selected = pool_page(queue, counts);
        if (!selected) return std::unexpected(selected.error());
        auto page = std::move(*selected);
        const auto& logical = queue.owner->device.logical_device();
        auto allocated = logical.allocateDescriptorSets(vk::DescriptorSetAllocateInfo{*page->pool, 1, &*layout.state_->sets[set_index]});
        state->set = std::move(allocated.front());
        ++page->allocations;
        state->page = std::move(page);
        for (auto& write : native_writes) write.dstSet = *state->set;
        logical.updateDescriptorSets(native_writes, {});
    } catch (const vk::SystemError& error) { return std::unexpected(queue.failure("create immutable bindings", static_cast<VkResult>(error.code().value()))); }
    if (auto status = detail::object_creation_status(queue, "create immutable bindings"); !status) return std::unexpected(status.error());
    return BindingSet{std::move(state)};
}
} // namespace dk::graphics

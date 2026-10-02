#pragma once
#include "ObjectInternal.hpp"

namespace dk::graphics::detail {
// Private bridge between device and presentation. Generic consumers cannot
// import swapchain handles or submit a WSI batch without its semaphore protocol.
struct PresentationAccess {
    static const auto& state(const SubmissionQueue& queue) { return queue.state_; }
    static void protect(CommandBatch& batch, std::shared_ptr<void> sync) {
        batch.state_->external_sync = true;
        batch.state_->external_sync_owner = std::move(sync);
    }
    static Image import_image(SubmissionQueue& queue, CommandBatch& batch, vk::Image native,
        const ImageDesc& desc, std::shared_ptr<void> owner) {
        auto state = memory::make_shared_in<ResourceState>(queue.state_->resource, queue.state_->owner, queue.state_->resource);
        state->states.resize(1);
        state->image_desc = desc;
        state->external_owner = std::move(owner);
        state->external_batch = batch.state_;
        state->image = static_cast<VkImage>(native);
        return Image{std::move(state)};
    }
    static void revoke(const Image& image) noexcept {
        if (!image.state_) return;
        if (auto batch = image.state_->external_batch.lock()) batch->invalid = true;
        image.state_->external_batch.reset();
    }
    static Result<void> export_present(CommandBatch& batch, const Image& image) {
        if (!batch.state_) return std::unexpected(Error{ErrorCode::invalid_state, "frame batch was moved or consumed"});
        auto& recording = *batch.state_;
        if (auto valid = recording.check(image.state_); !valid) return valid;
        if (recording.rendering) return std::unexpected(Error{ErrorCode::invalid_state, "end rendering before presenting"});
        auto* use = recording.find(image.state_);
        if (!use || !use->states.front().initialized)
            return std::unexpected(Error{ErrorCode::invalid_state, "present requires a fully initialized stored color image"});
        auto& state = use->states.front();
        const vk::ImageMemoryBarrier2 barrier{state.stages, state.access, {}, {}, state.layout,
            vk::ImageLayout::ePresentSrcKHR, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
            image.handle(), {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}};
        vk::DependencyInfo dependency{};
        dependency.setImageMemoryBarriers(barrier);
        recording.command().pipelineBarrier2(dependency);
        state = {{}, {}, vk::ImageLayout::ePresentSrcKHR, true};
        use->prepared.front() = 0;
        return {};
    }
    static Result<Submission> submit(SubmissionQueue& queue, CommandBatch&& batch, vk::Semaphore wait, vk::Semaphore signal) {
        if (!wait || !signal || !batch.state_ || !batch.state_->external_sync)
            return std::unexpected(Error{ErrorCode::invalid_argument, "WSI submit requires a protected batch and binary synchronization"});
        return queue.submit_impl(std::move(batch), wait, signal);
    }
};
} // namespace dk::graphics::detail

#include "PresentationInternal.hpp"
#include "PresentationPolicy.hpp"
#include "PresentationBridge.hpp"
#include <stdexcept>
#include <cstdio>

namespace dk::graphics::detail {
namespace { constexpr auto forever = std::numeric_limits<std::uint64_t>::max(); }
struct Generation {
    Generation(std::shared_ptr<DeviceLifetime> value, memory::ResourceHandle resource)
        : owner(std::move(value)), images(memory::Allocator<vk::Image>{resource}) {}
    std::shared_ptr<DeviceLifetime> owner;
    vk::raii::SwapchainKHR swapchain{nullptr};
    Vector<vk::Image> images;
    SwapchainChoice choice{};
};
struct FrameSync {
    explicit FrameSync(std::shared_ptr<DeviceLifetime> value) : owner(std::move(value)) {}
    std::shared_ptr<DeviceLifetime> owner;
    vk::raii::Semaphore acquire{nullptr}, finished{nullptr};
    vk::raii::Fence acquired{nullptr}, presented{nullptr};
    Submission render;
    bool present_pending = false, acquire_pending = false;
};
struct PresenterState {
    PresenterState(memory::ResourceHandle memory, platform::Window value, SubmissionQueue&& submission)
        : resource(std::move(memory)), window(std::move(value)), queue(std::move(submission)),
          slots(memory::Allocator<std::shared_ptr<FrameSync>>{resource}) {}
    memory::ResourceHandle resource;
    platform::Window window;
    SubmissionQueue queue;
    Vector<std::shared_ptr<FrameSync>> slots;
    std::shared_ptr<Generation> generation;
    PresentationApi api;
    PresentationStats stats;
    vk::Extent2D window_pixels{};
    std::size_t next_slot = 0;
    bool active = false, unknown_present = false;
    VkResult after_swapchain = VK_SUCCESS;

    const Device& device() const noexcept { return queue.device(); }
    bool lost() const noexcept { return queue.stats().device_lost; }
    Error failure(const char* operation, VkResult result) {
        if (result == VK_ERROR_DEVICE_LOST || result == VK_ERROR_SURFACE_LOST_KHR) stats.failed = true;
        return PresentationAccess::state(queue)->failure(operation, result);
    }
    Result<void> accepting() const {
        if (auto status = window.status(); !status) return std::unexpected(status.error());
        if (stats.closed || stats.failed) return std::unexpected(Error{ErrorCode::invalid_state, "presenter is closed or failed"});
        return PresentationAccess::state(queue)->accepting();
    }
    Result<bool> wait_fence(vk::Fence fence, std::uint64_t timeout) {
        const auto native = static_cast<VkFence>(fence);
        const auto result = api.wait(device().native_device(), 1, &native, VK_TRUE, timeout);
        if (result == VK_TIMEOUT) return false;
        if (result != VK_SUCCESS) return std::unexpected(failure("wait presentation fence", result));
        return true;
    }
    Result<bool> drain_slot(FrameSync& slot, std::uint64_t timeout) {
        if (slot.render.value()) {
            auto complete = queue.wait(slot.render, timeout);
            if (!complete || !*complete) return complete;
            slot.render = {};
        }
        if (slot.acquire_pending) {
            auto complete = wait_fence(*slot.acquired, timeout);
            if (!complete || !*complete) return complete;
            slot.acquire_pending = false;
        }
        if (slot.present_pending) {
            auto complete = wait_fence(*slot.presented, timeout);
            if (!complete || !*complete) return complete;
            slot.present_pending = false;
        }
        return true;
    }
    Result<void> drain() {
        if (unknown_present) return std::unexpected(Error{ErrorCode::internal_error, "unknown presentation completion state"});
        if (lost()) return {};
        for (auto& slot : slots) {
            auto complete = drain_slot(*slot, forever);
            if (!complete) return std::unexpected(complete.error());
        }
        return {};
    }
    Result<void> rebuild(vk::Extent2D pixels) {
        if (auto complete = drain(); !complete) return complete;
        try {
            const auto surface = device().surface();
            const auto caps = device().physical_device().getSurfaceCapabilitiesKHR(surface);
            const auto formats = device().physical_device().getSurfaceFormatsKHR(surface);
            auto choice = choose_swapchain(caps, formats, pixels);
            if (!choice) return std::unexpected(choice.error());
            auto candidate = memory::make_shared_in<Generation>(resource, PresentationAccess::state(queue)->owner, resource);
            candidate->choice = *choice;
            vk::SwapchainCreateInfoKHR info{};
            info.surface = surface;
            info.minImageCount = choice->images;
            info.imageFormat = choice->format.format;
            info.imageColorSpace = choice->format.colorSpace;
            info.imageExtent = choice->extent;
            info.imageArrayLayers = 1;
            info.imageUsage = choice->usage;
            info.imageSharingMode = vk::SharingMode::eExclusive;
            info.preTransform = caps.currentTransform;
            info.compositeAlpha = choice->alpha;
            info.presentMode = vk::PresentModeKHR::eFifo;
            info.clipped = VK_TRUE;
            // Passing oldSwapchain retires it even if creation fails. Publish no
            // partially constructed generation; next attempt starts without it.
            auto retired = std::move(generation);
            if (retired) info.oldSwapchain = *retired->swapchain;
            stats.needs_rebuild = true;
            VkSwapchainKHR native = VK_NULL_HANDLE;
            const auto result = api.create(device().native_device(), reinterpret_cast<const VkSwapchainCreateInfoKHR*>(&info), nullptr, &native);
            if (result != VK_SUCCESS) return std::unexpected(failure("vkCreateSwapchainKHR", result));
            candidate->swapchain = vk::raii::SwapchainKHR{device().logical_device(), native};
            if (const auto injected = std::exchange(after_swapchain, VK_SUCCESS); injected != VK_SUCCESS)
                return std::unexpected(failure("swapchain generation construction", injected));
            const auto images = candidate->swapchain.getImages();
            if (images.empty()) return std::unexpected(Error{ErrorCode::internal_error, "swapchain has no images"});
            candidate->images.assign(images.begin(), images.end());
            generation = std::move(candidate);
            window_pixels = pixels;
            ++stats.generation;
            stats.needs_rebuild = false;
            return {};
        } catch (const vk::SystemError& error) {
            return std::unexpected(failure("create swapchain generation", static_cast<VkResult>(error.code().value())));
        }
    }
    ~PresenterState() {
        if (!window.status()) std::terminate();
        if (auto done = drain(); !done && !lost()) {
            std::fprintf(stderr, "presentation teardown: %s\n", done.error().message.c_str());
            std::terminate(); // Never destroy resources with unproven completion.
        }
        generation.reset();
    }
};
struct FrameState {
    explicit FrameState(std::shared_ptr<PresenterState> value) : owner(std::move(value)) {}
    std::shared_ptr<PresenterState> owner;
    std::shared_ptr<Generation> generation;
    std::shared_ptr<FrameSync> sync;
    std::uint32_t image_index = 0;
    Image image;
    ImageView view;
    CommandBatch batch;
    bool acquired = false, submitted = false;
    ~FrameState() {
        PresentationAccess::revoke(image);
        batch = {};
        if (!acquired) return;
        if (!owner->window.status()) std::terminate();
        // No present was queued. Successful submit (present OOM case) must
        // finish before releasing the image or destroying its signal semaphore.
        if (!owner->lost()) {
            auto done = owner->drain_slot(*sync, forever);
            if (!done && !owner->lost()) std::terminate();
        }
        if (!owner->lost()) {
            auto done = owner->wait_fence(*sync->acquired, forever);
            if (!done && !owner->lost()) std::terminate();
        }
        if (!owner->lost()) {
            const VkReleaseSwapchainImagesInfoEXT info{VK_STRUCTURE_TYPE_RELEASE_SWAPCHAIN_IMAGES_INFO_EXT,
                nullptr, static_cast<VkSwapchainKHR>(*generation->swapchain), 1, &image_index};
            const auto result = owner->api.release(owner->device().native_device(), &info);
            if (result != VK_SUCCESS) {
                owner->stats.failed = true;
                if (result == VK_ERROR_DEVICE_LOST) PresentationAccess::state(owner->queue)->owner->lost = true;
            }
        }
        sync->acquire.clear(); // Acquire fence proves no signal operation remains.
        if (submitted) sync->finished.clear();
        owner->active = false;
        ++owner->stats.abandoned;
    }
};
PresentationApi PresenterAccess::api(const Presenter& presenter) { return presenter.state_->api; }
void PresenterAccess::set_api(Presenter& presenter, PresentationApi api) { presenter.state_->api = api; }
void PresenterAccess::fail_after_swapchain(Presenter& presenter, VkResult result) { presenter.state_->after_swapchain = result; }
} // namespace dk::graphics::detail

namespace dk::graphics {
Frame::Frame() = default;
Frame::Frame(memory::UniquePtr<detail::FrameState> state) : state_(std::move(state)) {}
Frame::~Frame() = default;
Frame::Frame(Frame&&) noexcept = default;
Frame& Frame::operator=(Frame&&) noexcept = default;
Frame::operator bool() const noexcept { return state_ && state_->acquired; }
CommandBatch& Frame::commands() { if (!*this) throw std::logic_error("frame is empty or consumed"); return state_->batch; }
const Image& Frame::color() const { if (!*this) throw std::logic_error("frame is empty or consumed"); return state_->image; }
const ImageView& Frame::color_view() const { if (!*this) throw std::logic_error("frame is empty or consumed"); return state_->view; }
vk::Extent2D Frame::extent() const { if (!*this) throw std::logic_error("frame is empty or consumed"); return state_->generation->choice.extent; }
vk::Format Frame::format() const { if (!*this) throw std::logic_error("frame is empty or consumed"); return state_->generation->choice.format.format; }

Result<Presenter> Presenter::create(memory::ResourceHandle resource, platform::Window window,
    const DeviceOptions& options, const PresentationOptions& presentation)
{
    if (!presentation.frames_in_flight || presentation.frames_in_flight > 8)
        return std::unexpected(Error{ErrorCode::invalid_argument, "presentation requires 1..8 frame slots"});
    auto device = create_present_device(resource, window, options);
    if (!device) return std::unexpected(device.error());
    auto queue = SubmissionQueue::create(resource, std::move(*device), presentation.frames_in_flight + 1);
    if (!queue) return std::unexpected(queue.error());
    auto state = memory::make_shared_in<detail::PresenterState>(resource, resource, std::move(window), std::move(*queue));
    const auto& logical = state->device().logical_device();
    const auto* dispatch = logical.getDispatcher();
    state->api = {dispatch->vkCreateSwapchainKHR, dispatch->vkAcquireNextImageKHR, dispatch->vkQueuePresentKHR,
        dispatch->vkReleaseSwapchainImagesEXT, dispatch->vkWaitForFences};
    if (!state->api.create || !state->api.acquire || !state->api.present || !state->api.release || !state->api.wait)
        return std::unexpected(Error{ErrorCode::not_supported, "presentation device is missing WSI entry points"});
    try {
        state->slots.reserve(presentation.frames_in_flight);
        for (std::uint32_t i = 0; i < presentation.frames_in_flight; ++i) {
            auto sync = memory::make_shared_in<detail::FrameSync>(resource, detail::PresentationAccess::state(state->queue)->owner);
            sync->acquire = vk::raii::Semaphore{logical, vk::SemaphoreCreateInfo{}};
            sync->finished = vk::raii::Semaphore{logical, vk::SemaphoreCreateInfo{}};
            sync->acquired = vk::raii::Fence{logical, vk::FenceCreateInfo{}};
            sync->presented = vk::raii::Fence{logical, vk::FenceCreateInfo{}};
            state->slots.push_back(std::move(sync));
        }
    } catch (const vk::SystemError& error) {
        return std::unexpected(state->failure("create frame synchronization", static_cast<VkResult>(error.code().value())));
    }
    return Presenter{std::move(state)};
}
Presenter::Presenter(std::shared_ptr<detail::PresenterState> state) : state_(std::move(state)) {}
Presenter::~Presenter() = default;
Presenter::Presenter(Presenter&&) noexcept = default;
Presenter& Presenter::operator=(Presenter&&) noexcept = default;
SubmissionQueue& Presenter::queue() noexcept { return state_->queue; }
PresentationStats Presenter::stats() const noexcept {
    auto result = state_->stats;
    result.failed = result.failed || state_->lost();
    return result;
}
Result<void> Presenter::request_rebuild() {
    if (auto valid = state_->accepting(); !valid) return valid;
    state_->stats.needs_rebuild = true;
    return {};
}
Result<AcquiredFrame> Presenter::acquire(std::uint64_t timeout)
{
    if (auto valid = state_->accepting(); !valid) return std::unexpected(valid.error());
    if (timeout == std::numeric_limits<std::uint64_t>::max())
        return std::unexpected(Error{ErrorCode::invalid_argument, "acquire timeout must be finite"});
    if (state_->resource.state() != memory::ResourceState::open)
        return std::unexpected(Error{ErrorCode::invalid_state, "presentation Memory is closing"});
    if (state_->active) return std::unexpected(Error{ErrorCode::conflict, "present or release the active frame before acquiring"});
    const auto status = state_->window.status();
    if (!status) return std::unexpected(status.error());
    if (status->close_requested) return std::unexpected(Error{ErrorCode::invalid_state, "window close was requested"});
    const vk::Extent2D pixels{status->pixel_width, status->pixel_height};
    if (!pixels.width || !pixels.height) {
        state_->stats.needs_rebuild = true;
        return AcquiredFrame{AcquireStatus::suspended, {}};
    }
    if (pixels != state_->window_pixels) state_->stats.needs_rebuild = true;
    if (state_->stats.needs_rebuild || !state_->generation) {
        if (auto rebuilt = state_->rebuild(pixels); !rebuilt) {
            if (rebuilt.error().code == ErrorCode::invalid_state) return AcquiredFrame{AcquireStatus::suspended, {}};
            return std::unexpected(rebuilt.error());
        }
    }
    auto sync = state_->slots[state_->next_slot];
    if (auto ready = state_->drain_slot(*sync, timeout); !ready) return std::unexpected(ready.error());
    else if (!*ready) return AcquiredFrame{};
    try {
        const auto& device = state_->device().logical_device();
        if (!*sync->acquire) sync->acquire = vk::raii::Semaphore{device, vk::SemaphoreCreateInfo{}};
        if (!*sync->finished) sync->finished = vk::raii::Semaphore{device, vk::SemaphoreCreateInfo{}};
        device.resetFences(std::array<vk::Fence, 2>{*sync->acquired, *sync->presented});
        auto frame = memory::make_unique_in<detail::FrameState>(state_->resource, state_);
        frame->generation = state_->generation;
        frame->sync = sync;
        auto batch = state_->queue.begin();
        if (!batch) return std::unexpected(batch.error());
        frame->batch = std::move(*batch);
        detail::PresentationAccess::protect(frame->batch, sync);
        const auto result = state_->api.acquire(state_->device().native_device(), static_cast<VkSwapchainKHR>(*state_->generation->swapchain),
            timeout, static_cast<VkSemaphore>(*sync->acquire), static_cast<VkFence>(*sync->acquired), &frame->image_index);
        if (result == VK_TIMEOUT || result == VK_NOT_READY) return AcquiredFrame{};
        if (result == VK_ERROR_OUT_OF_DATE_KHR) { state_->stats.needs_rebuild = true; return AcquiredFrame{}; }
        if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) return std::unexpected(state_->failure("vkAcquireNextImageKHR", result));
        // Acquisition commit precedes every possibly throwing allocation below.
        state_->active = frame->acquired = true;
        sync->acquire_pending = true;
        state_->next_slot = (state_->next_slot + 1) % state_->slots.size();
        if (result == VK_SUBOPTIMAL_KHR) state_->stats.needs_rebuild = true;
        const auto& choice = state_->generation->choice;
        frame->image = detail::PresentationAccess::import_image(state_->queue, frame->batch,
            state_->generation->images.at(frame->image_index),
            {choice.extent.width, choice.extent.height, choice.format.format, choice.usage}, state_->generation);
        auto view = state_->queue.resources().create_view(frame->image);
        if (!view) return std::unexpected(view.error());
        frame->view = std::move(*view);
        return AcquiredFrame{AcquireStatus::ready, Frame{std::move(frame)}};
    } catch (const vk::SystemError& error) {
        return std::unexpected(state_->failure("acquire frame resources", static_cast<VkResult>(error.code().value())));
    }
}
Result<PresentResult> Presenter::present(Frame&& frame)
{
    if (auto valid = state_->accepting(); !valid) return std::unexpected(valid.error());
    if (!frame || frame.state_->owner != state_)
        return std::unexpected(Error{ErrorCode::invalid_argument, "frame is empty or belongs to another presenter"});
    if (auto exported = detail::PresentationAccess::export_present(frame.state_->batch, frame.state_->image); !exported)
        return std::unexpected(exported.error());
    auto current = std::move(frame.state_);
    auto& sync = *current->sync;
    auto ticket = detail::PresentationAccess::submit(state_->queue, std::move(current->batch), *sync.acquire, *sync.finished);
    if (!ticket) return std::unexpected(ticket.error());
    sync.render = *ticket;
    current->submitted = true;
    ++state_->stats.submitted;
    const VkSwapchainKHR swapchain = static_cast<VkSwapchainKHR>(*current->generation->swapchain);
    const VkSemaphore wait = static_cast<VkSemaphore>(*sync.finished);
    const VkFence fence = static_cast<VkFence>(*sync.presented);
    const VkSwapchainPresentFenceInfoEXT completion{VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_EXT, nullptr, 1, &fence};
    const VkPresentInfoKHR info{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, &completion, 1, &wait, 1, &swapchain, &current->image_index, nullptr};
    const auto result = state_->api.present(static_cast<VkQueue>(*state_->device().queue()), &info);
    if (detail::present_enqueued(result)) {
        sync.present_pending = true;
        current->acquired = false;
        state_->active = false;
        if (result == VK_SUBOPTIMAL_KHR || result == VK_ERROR_OUT_OF_DATE_KHR) state_->stats.needs_rebuild = true;
        if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR) ++state_->stats.presented;
    } else if (result != VK_ERROR_OUT_OF_HOST_MEMORY && result != VK_ERROR_OUT_OF_DEVICE_MEMORY && result != VK_ERROR_DEVICE_LOST) {
        // An unknown driver status cannot establish whether presentation owns
        // the semaphore. Preserve resources and refuse unsafe teardown.
        state_->unknown_present = state_->stats.failed = true;
        current->acquired = false;
        state_->active = false;
    }
    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR && result != VK_ERROR_OUT_OF_DATE_KHR)
        return std::unexpected(state_->failure("vkQueuePresentKHR", result));
    return PresentResult{state_->stats.needs_rebuild ? PresentStatus::needs_rebuild : PresentStatus::presented, *ticket};
}
Result<void> Presenter::close()
{
    if (auto status = state_->window.status(); !status) return std::unexpected(status.error());
    if (state_->stats.closed) return {};
    if (state_->active) return std::unexpected(Error{ErrorCode::conflict, "release active frame before closing Presenter"});
    if (auto done = state_->drain(); !done) return done;
    if (auto closed = state_->queue.close(); !closed && !state_->lost()) return closed;
    state_->generation.reset();
    state_->stats.closed = true;
    if (state_->lost()) return std::unexpected(Error{ErrorCode::invalid_state, "closed a lost presentation device"});
    return {};
}
} // namespace dk::graphics

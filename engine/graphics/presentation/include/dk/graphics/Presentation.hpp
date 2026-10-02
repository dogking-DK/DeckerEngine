#pragma once
#include <dk/graphics/PresentDevice.hpp>
#include <dk/graphics/CommandEncoder.hpp>

namespace dk::graphics {
namespace detail { struct PresenterState; struct FrameState; struct PresenterAccess; }
struct PresentationOptions { std::uint32_t frames_in_flight = 2; };
struct PresentationStats {
    std::uint64_t generation = 0, submitted = 0, presented = 0, abandoned = 0;
    bool needs_rebuild = true, closed = false, failed = false;
};
class Frame final {
public:
    Frame();
    ~Frame(); // Releases an acquired but unsubmitted image; may wait for acquire.
    Frame(Frame&&) noexcept;
    Frame& operator=(Frame&&) noexcept;
    Frame(const Frame&) = delete;
    Frame& operator=(const Frame&) = delete;
    [[nodiscard]] explicit operator bool() const noexcept;
    // Borrowed only while this Frame is active. Do not move/submit the batch.
    [[nodiscard]] CommandBatch& commands();
    [[nodiscard]] const Image& color() const;
    [[nodiscard]] const ImageView& color_view() const;
    [[nodiscard]] vk::Extent2D extent() const;
    [[nodiscard]] vk::Format format() const;
private:
    friend class Presenter;
    explicit Frame(memory::UniquePtr<detail::FrameState> state);
    memory::UniquePtr<detail::FrameState> state_;
};
enum class AcquireStatus { ready, retry, suspended };
struct AcquiredFrame { AcquireStatus status = AcquireStatus::retry; Frame frame; };
enum class PresentStatus { presented, needs_rebuild };
struct PresentResult { PresentStatus status; Submission completion; };
class Presenter final {
public:
    [[nodiscard]] static Result<Presenter> create(memory::ResourceHandle resource, platform::Window window,
        const DeviceOptions& device = {}, const PresentationOptions& options = {});
    ~Presenter();
    Presenter(Presenter&&) noexcept;
    Presenter& operator=(Presenter&&) noexcept;
    Presenter(const Presenter&) = delete;
    Presenter& operator=(const Presenter&) = delete;
    // Borrowed queue. Use for factories and independent uploads; do not move it.
    [[nodiscard]] SubmissionQueue& queue() noexcept;
    [[nodiscard]] Result<AcquiredFrame> acquire(std::uint64_t timeout_ns = 16'000'000);
    // Consumes a local active frame after validation, including on submit/present failure.
    [[nodiscard]] Result<PresentResult> present(Frame&& frame);
    [[nodiscard]] Result<void> request_rebuild();
    [[nodiscard]] Result<void> close();
    [[nodiscard]] PresentationStats stats() const noexcept;
private:
    friend struct detail::PresenterAccess;
    explicit Presenter(std::shared_ptr<detail::PresenterState> state);
    std::shared_ptr<detail::PresenterState> state_;
};
} // namespace dk::graphics

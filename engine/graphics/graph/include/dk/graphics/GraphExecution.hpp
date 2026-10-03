#pragma once
#include <dk/graphics/Graph.hpp>
#include <dk/graphics/CommandEncoder.hpp>

namespace dk::graphics::graph {
namespace detail { struct ExecutionState; }
struct ExternalBinding {
    std::size_t resource;
    const Buffer* buffer = nullptr;
    const Image* image = nullptr;
    // Empty imports the live ledger; otherwise exact expected states, layer-major/mip-minor.
    std::span<const AccessState> expected_states;
};
struct FinalAccess {
    std::size_t resource;
    AccessDescription access;
};
struct ExportedState {
    std::size_t resource;
    std::uint32_t mip = 0, layer = 0;
    AccessState state;
};
struct Synchronization {
    std::optional<std::size_t> pass; // Empty for final access; otherwise declaration index.
    std::size_t resource;
    std::uint32_t mip = 0, layer = 0;
    AccessState before, target; // Actual barrier scopes; initialized is preserved.
};
// Borrowed for one callback only. Declare every accessed binding/range in the Pass.
// Encoders must end within the callback; no submission or manual barriers are exposed.
class PassContext final {
public:
    PassContext(const PassContext&) = delete;
    PassContext& operator=(const PassContext&) = delete;
    [[nodiscard]] std::size_t pass_index() const noexcept { return pass_; }
    [[nodiscard]] const PlannedPass& description() const noexcept;
    [[nodiscard]] Result<const Buffer*> buffer(std::size_t resource) const;
    [[nodiscard]] Result<const Image*> image(std::size_t resource) const;
    [[nodiscard]] Result<void> copy_buffer(std::size_t source, std::size_t destination, vk::DeviceSize size,
        vk::DeviceSize source_offset = 0, vk::DeviceSize destination_offset = 0);
    [[nodiscard]] Result<void> copy_to_image(std::size_t source, std::size_t destination, const ImageCopyRegion& region);
    [[nodiscard]] Result<void> copy_to_buffer(std::size_t source, std::size_t destination, const ImageCopyRegion& region);
    [[nodiscard]] Result<void> fill(std::size_t resource, std::uint32_t value = 0);
    [[nodiscard]] Result<void> clear(std::size_t resource, const vk::ClearColorValue& color, const vk::ImageSubresourceRange& range);
    [[nodiscard]] Result<ComputeEncoder> compute();
    [[nodiscard]] Result<RenderEncoder> begin_rendering(const RenderingDesc& description);
private:
    friend struct detail::ExecutionAccess;
    PassContext(const CompiledGraph& plan, detail::ExecutionState& state, CommandBatch& batch, std::size_t pass)
        : plan_(plan), state_(state), batch_(batch), pass_(pass) {}
    [[nodiscard]] bool buffer_range(std::size_t resource, vk::DeviceSize offset, vk::DeviceSize size) const;
    [[nodiscard]] bool image_range(std::size_t resource, const vk::ImageSubresourceRange& range) const;
    const CompiledGraph& plan_;
    detail::ExecutionState& state_;
    CommandBatch& batch_;
    std::size_t pass_;
};
using PassRecorder = Result<void> (*)(PassContext&, void*);
struct PassCallback {
    std::size_t pass;
    PassRecorder record = nullptr;
    void* user_data = nullptr;
};
struct ExecutionDesc {
    std::span<const ExternalBinding> bindings;
    std::span<const PassCallback> callbacks;
    std::span<const FinalAccess> final_accesses;
    bool capture_synchronization = false;
};
class Execution final {
public:
    Execution() = default;
    Execution(Execution&&) noexcept = default;
    Execution& operator=(Execution&&) noexcept = default;
    Execution(const Execution&) = delete;
    Execution& operator=(const Execution&) = delete;
    [[nodiscard]] explicit operator bool() const noexcept { return bool(state_); }
    // Use queue.wait/poll; publication of states does not imply GPU completion.
    [[nodiscard]] Submission submission() const noexcept;
    [[nodiscard]] std::span<const ExportedState> states() const noexcept;
    [[nodiscard]] std::span<const Synchronization> synchronization() const noexcept;
    // Output owners only. Borrowed until this result is replaced/destroyed; share() keeps an owner.
    [[nodiscard]] Result<const Buffer*> buffer(std::size_t resource) const;
    [[nodiscard]] Result<const Image*> image(std::size_t resource) const;
private:
    friend struct detail::ExecutionAccess;
    explicit Execution(std::shared_ptr<detail::ExecutionState> state) : state_(std::move(state)) {}
    std::shared_ptr<detail::ExecutionState> state_;
};
// Synchronous CPU recording, asynchronous single-queue GPU work. No hidden waits.
// Callback CPU side effects are not rolled back when recording/submission fails.
[[nodiscard]] Result<Execution> execute(const CompiledGraph& plan, SubmissionQueue& queue, const ExecutionDesc& description);
} // namespace dk::graphics::graph

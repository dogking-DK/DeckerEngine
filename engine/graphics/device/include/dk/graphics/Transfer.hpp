#pragma once
#include <dk/graphics/CommandEncoder.hpp>

namespace dk::graphics {
namespace detail { struct ReadbackState; }
enum class ReadbackStatus { unsubmitted, pending, ready, cancelled, device_lost };
struct ReadbackDescription {
    vk::DeviceSize bytes = 0;
    vk::Format format = vk::Format::eUndefined; // Undefined for a buffer slice.
    std::uint32_t width = 0, height = 0;
    vk::DeviceSize row_pitch = 0; // Image results are tightly packed.
};
class ReadbackRequest final {
public:
    ReadbackRequest() = default;
    ReadbackRequest(ReadbackRequest&&) noexcept = default;
    ReadbackRequest& operator=(ReadbackRequest&&) noexcept = default;
    ReadbackRequest(const ReadbackRequest&) = delete;
    ReadbackRequest& operator=(const ReadbackRequest&) = delete;
    [[nodiscard]] ReadbackStatus status() const noexcept;
    [[nodiscard]] ReadbackDescription description() const noexcept;
    // false = not submitted or not collected yet; no bytes are written.
    // The destination must exactly match description().bytes. Queue poll/wait advances completion.
    [[nodiscard]] Result<bool> try_read(std::span<std::byte> destination) const;
private:
    friend class CommandBatch;
    explicit ReadbackRequest(std::shared_ptr<detail::ReadbackState> state) : state_(std::move(state)) {}
    std::shared_ptr<detail::ReadbackState> state_;
};
} // namespace dk::graphics

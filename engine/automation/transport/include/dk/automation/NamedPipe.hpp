#pragma once
#include <chrono>
#include <cstddef>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace dk::ipc
{
inline constexpr std::size_t max_request_bytes = 1024 * 1024;
inline constexpr std::size_t max_response_bytes = 8 * 1024 * 1024;
using Deadline = std::chrono::steady_clock::time_point;
enum class Failure { timeout, disconnected, invalid, system, stopped };
struct PipeError { Failure kind; std::string message; };
template<class T> using PipeResult = std::expected<T, PipeError>;
[[nodiscard]] bool valid_endpoint(std::string_view name);

class Connection final
{
  public:
    ~Connection();
    Connection(Connection&&) noexcept;
    Connection& operator=(Connection&&) noexcept;
    [[nodiscard]] static PipeResult<Connection> connect(std::string_view name, Deadline deadline);
    [[nodiscard]] PipeResult<void> send(std::string_view bytes, Deadline deadline);
    [[nodiscard]] PipeResult<std::string> receive(Deadline deadline, std::size_t limit = max_response_bytes);
  private:
    struct Impl;
    explicit Connection(std::unique_ptr<Impl>);
    std::unique_ptr<Impl> impl_;
};
struct PendingFrame;
class Incoming final
{
  public:
    [[nodiscard]] const std::string& bytes() const;
    void reply(std::string bytes);
  private:
    friend class PipeServer;
    explicit Incoming(std::shared_ptr<PendingFrame> frame) : frame_{std::move(frame)} {}
    std::shared_ptr<PendingFrame> frame_;
};
struct PipeOptions
{
    std::size_t queued = 8;
    std::chrono::milliseconds timeout{5000};
};
class PipeServer final
{
  public:
    ~PipeServer();
    [[nodiscard]] static PipeResult<std::unique_ptr<PipeServer>> listen(
        std::string_view name, std::function<void()> wake, const PipeOptions& options = {});
    // Owner thread only. Accepted frames survive a peer disconnect.
    [[nodiscard]] std::optional<Incoming> pop();
    void close(std::chrono::milliseconds grace = std::chrono::milliseconds{0}) noexcept;
  private:
    struct Impl;
    explicit PipeServer(std::unique_ptr<Impl>);
    std::unique_ptr<Impl> impl_;
};
} // namespace dk::ipc

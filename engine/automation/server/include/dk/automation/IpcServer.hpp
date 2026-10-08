#pragma once
#include <dk/automation/NamedPipe.hpp>
#include <dk/automation/RpcRuntime.hpp>
#include <map>
#include <thread>

namespace dk
{
struct IpcSessionOptions
{
    std::size_t retained = 256;
    std::size_t bytes = 32 * 1024 * 1024;
};
// Owner-thread session ledger. A missing ticket is never interpreted as a new request.
class IpcSession final
{
  public:
    explicit IpcSession(Runtime&, IpcSessionOptions options = {});
    [[nodiscard]] const std::string& id() const { return id_; }
    [[nodiscard]] std::uint64_t dispatch_count() const { return dispatches_; }
    [[nodiscard]] std::string handle(std::string_view frame);
  private:
    struct Entry { std::string request; std::string response; };
    Runtime& runtime_;
    IpcSessionOptions options_;
    std::thread::id owner_ = std::this_thread::get_id();
    std::string id_;
    std::int64_t next_ = 1;
    std::map<std::int64_t, Entry> entries_;
    std::size_t bytes_ = 0;
    std::uint64_t dispatches_ = 0;
    void evict(std::int64_t except = 0);
    [[nodiscard]] std::string wrap(Json response) const;
    [[nodiscard]] std::string reject(Json id, int code, std::string message, bool unknown = false) const;
};
class IpcServer final
{
  public:
    [[nodiscard]] static Result<std::unique_ptr<IpcServer>> listen(Runtime&, std::string_view name,
        const ipc::PipeOptions& pipe_options = {}, IpcSessionOptions session_options = {});
    [[nodiscard]] std::size_t pump(std::size_t budget = 8);
    [[nodiscard]] std::uint64_t dispatch_count() const { return session_.dispatch_count(); }
    void close(std::chrono::milliseconds grace = std::chrono::milliseconds{0}) noexcept;
  private:
    IpcServer(Runtime& runtime, IpcSessionOptions options) : session_{runtime, options} {}
    IpcSession session_;
    std::unique_ptr<ipc::PipeServer> pipe_;
};
} // namespace dk

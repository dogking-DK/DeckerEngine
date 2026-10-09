#include <dk/automation/NamedPipe.hpp>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <sddl.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace dk::ipc
{
namespace
{
struct Handle
{
    HANDLE value = INVALID_HANDLE_VALUE;
    explicit Handle(HANDLE v = INVALID_HANDLE_VALUE) : value{v} {}
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& other) noexcept : value{std::exchange(other.value, INVALID_HANDLE_VALUE)} {}
};
struct Local { void* value{}; ~Local() { if (value) LocalFree(value); } };
PipeError failure(std::string_view operation, DWORD code = GetLastError())
{
    const auto kind = code == ERROR_BROKEN_PIPE || code == ERROR_NO_DATA || code == ERROR_PIPE_NOT_CONNECTED
        ? Failure::disconnected : Failure::system;
    return {kind, std::string{operation} + " (Win32 " + std::to_string(code) + ")"};
}
DWORD remaining(Deadline deadline)
{
    if (deadline == Deadline::max()) return INFINITE;
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) return 0;
    const auto ms = std::chrono::ceil<std::chrono::milliseconds>(deadline - now).count();
    return static_cast<DWORD>(std::min<std::int64_t>(ms, 0xfffffffe));
}
std::wstring path(std::string_view name)
{
    return L"\\\\.\\pipe\\DeckerEngine." + std::wstring{name.begin(), name.end()};
}
enum class Operation { connect, read, write };
PipeResult<DWORD> io(HANDLE pipe, HANDLE stop, Operation op, void* data, DWORD count, Deadline deadline)
{
    if (remaining(deadline) == 0) return std::unexpected(PipeError{Failure::timeout, "pipe deadline expired"});
    if (stop && WaitForSingleObject(stop, 0) == WAIT_OBJECT_0)
        return std::unexpected(PipeError{Failure::stopped, "pipe stopped"});
    Handle event{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    if (!event.value) return std::unexpected(failure("CreateEvent"));
    OVERLAPPED overlapped{};
    overlapped.hEvent = event.value;
    BOOL started = FALSE;
    if (op == Operation::connect) started = ConnectNamedPipe(pipe, &overlapped);
    else if (op == Operation::read) started = ReadFile(pipe, data, count, nullptr, &overlapped);
    else started = WriteFile(pipe, data, count, nullptr, &overlapped);
    if (!started)
    {
        const auto error = GetLastError();
        if (op == Operation::connect && error == ERROR_PIPE_CONNECTED) return 0;
        if (error != ERROR_IO_PENDING) return std::unexpected(failure("pipe IO", error));
        const HANDLE events[]{event.value, stop};
        const auto wait = WaitForMultipleObjects(stop ? 2 : 1, events, FALSE, remaining(deadline));
        if (wait != WAIT_OBJECT_0)
        {
            auto problem = wait == WAIT_TIMEOUT ? PipeError{Failure::timeout, "pipe deadline expired"}
                : wait == WAIT_OBJECT_0 + 1 ? PipeError{Failure::stopped, "pipe stopped"} : failure("pipe wait");
            CancelIoEx(pipe, &overlapped);
            DWORD ignored{};
            // The OVERLAPPED and its event must outlive even cancelled IO.
            GetOverlappedResult(pipe, &overlapped, &ignored, TRUE);
            return std::unexpected(std::move(problem));
        }
    }
    DWORD transferred{};
    if (!GetOverlappedResult(pipe, &overlapped, &transferred, FALSE)) return std::unexpected(failure("pipe completion"));
    if (op != Operation::connect && transferred == 0)
        return std::unexpected(PipeError{Failure::disconnected, "pipe closed"});
    return transferred;
}
PipeResult<void> transfer(HANDLE pipe, HANDLE stop, Operation op, char* bytes, std::size_t size, Deadline deadline)
{
    while (size)
    {
        auto count = io(pipe, stop, op, bytes, static_cast<DWORD>(std::min<std::size_t>(size, 65536)), deadline);
        if (!count) return std::unexpected(count.error());
        bytes += *count;
        size -= *count;
    }
    return {};
}
PipeResult<std::string> read_frame(HANDLE pipe, HANDLE stop, Deadline deadline, std::size_t limit)
{
    std::array<char, 4> header{};
    auto read = transfer(pipe, stop, Operation::read, header.data(), header.size(), deadline);
    if (!read) return std::unexpected(read.error());
    std::uint32_t size{};
    for (unsigned i = 0; i < 4; ++i) size |= static_cast<std::uint32_t>(static_cast<unsigned char>(header[i])) << (8 * i);
    if (!size || size > limit) return std::unexpected(PipeError{Failure::invalid, "invalid pipe frame length"});
    std::string bytes(size, '\0');
    read = transfer(pipe, stop, Operation::read, bytes.data(), bytes.size(), deadline);
    if (!read) return std::unexpected(read.error());
    return bytes;
}
PipeResult<void> write_frame(HANDLE pipe, HANDLE stop, std::string_view bytes, Deadline deadline)
{
    if (bytes.empty() || bytes.size() > max_response_bytes)
        return std::unexpected(PipeError{Failure::invalid, "invalid pipe frame length"});
    std::array<char, 4> header{};
    for (unsigned i = 0; i < 4; ++i) header[i] = static_cast<char>((bytes.size() >> (8 * i)) & 0xff);
    auto sent = transfer(pipe, stop, Operation::write, header.data(), header.size(), deadline);
    if (!sent) return sent;
    return transfer(pipe, stop, Operation::write, const_cast<char*>(bytes.data()), bytes.size(), deadline);
}
PipeResult<Handle> create_pipe(std::string_view name)
{
    HANDLE token_value{};
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token_value)) return std::unexpected(failure("OpenProcessToken"));
    Handle token{token_value};
    DWORD size{};
    GetTokenInformation(token.value, TokenUser, nullptr, 0, &size);
    if (!size) return std::unexpected(failure("GetTokenInformation size"));
    std::vector<std::byte> user(size);
    if (!GetTokenInformation(token.value, TokenUser, user.data(), size, &size)) return std::unexpected(failure("GetTokenInformation"));
    LPWSTR sid_value{};
    if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid, &sid_value))
        return std::unexpected(failure("ConvertSidToStringSid"));
    Local sid{sid_value};
    const std::wstring acl = L"D:P(A;;GA;;;" + std::wstring{sid_value} + L")";
    PSECURITY_DESCRIPTOR descriptor_value{};
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(acl.c_str(), SDDL_REVISION_1, &descriptor_value, nullptr))
        return std::unexpected(failure("pipe security descriptor"));
    Local descriptor{descriptor_value};
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), descriptor_value, FALSE};
    Handle pipe{CreateNamedPipeW(path(name).c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 65536, 65536, 0, &security)};
    if (pipe.value == INVALID_HANDLE_VALUE) return std::unexpected(failure("CreateNamedPipe"));
    return pipe;
}
} // namespace
bool valid_endpoint(std::string_view name)
{
    return !name.empty() && name.size() <= 64 && std::ranges::all_of(name, [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
    });
}
struct Connection::Impl { Handle pipe; explicit Impl(Handle handle) : pipe{std::move(handle)} {} };
Connection::Connection(std::unique_ptr<Impl> impl) : impl_{std::move(impl)} {}
Connection::~Connection() = default;
Connection::Connection(Connection&&) noexcept = default;
Connection& Connection::operator=(Connection&&) noexcept = default;
PipeResult<Connection> Connection::connect(std::string_view name, Deadline deadline)
{
    if (!valid_endpoint(name)) return std::unexpected(PipeError{Failure::invalid, "invalid pipe endpoint name"});
    const auto pipe_path = path(name);
    while (remaining(deadline))
    {
        Handle handle{CreateFileW(pipe_path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
            FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr)};
        if (handle.value != INVALID_HANDLE_VALUE) return Connection{std::make_unique<Impl>(std::move(handle))};
        const auto error = GetLastError();
        if (error != ERROR_PIPE_BUSY && error != ERROR_FILE_NOT_FOUND) return std::unexpected(failure("open pipe", error));
        const auto wait = remaining(deadline);
        if (!wait) break; // WaitNamedPipe(0) means the server's default timeout, not an expired deadline.
        if (error == ERROR_PIPE_BUSY) WaitNamedPipeW(pipe_path.c_str(), std::min<DWORD>(wait, 20));
        else std::this_thread::sleep_for(std::chrono::milliseconds{std::min<DWORD>(wait, 10)});
    }
    return std::unexpected(PipeError{Failure::timeout, "pipe connection deadline expired"});
}
PipeResult<void> Connection::send(std::string_view bytes, Deadline deadline)
{
    return write_frame(impl_->pipe.value, nullptr, bytes, deadline);
}
PipeResult<std::string> Connection::receive(Deadline deadline, std::size_t limit)
{
    return read_frame(impl_->pipe.value, nullptr, deadline, limit);
}
struct PendingFrame
{
    std::string bytes;
    std::mutex mutex;
    std::condition_variable ready;
    std::optional<std::string> response;
};
const std::string& Incoming::bytes() const { return frame_->bytes; }
void Incoming::reply(std::string bytes)
{
    if (bytes.empty() || bytes.size() > max_response_bytes) throw std::invalid_argument{"invalid pipe reply size"};
    std::lock_guard lock{frame_->mutex};
    if (frame_->response) throw std::logic_error{"pipe frame already replied"};
    frame_->response = std::move(bytes);
    frame_->ready.notify_all();
}
struct PipeServer::Impl
{
    Handle pipe;
    Handle accept_stop{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    Handle stop{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    PipeOptions options;
    std::function<void()> wake;
    std::jthread worker;
    std::atomic_bool closing = false, stopped = false;
    std::mutex mutex;
    std::condition_variable finished;
    std::deque<std::shared_ptr<PendingFrame>> queue;
    bool done = false;
    std::exception_ptr fatal;
    Impl(Handle handle, PipeOptions opts, std::function<void()> notify)
        : pipe{std::move(handle)}, options{opts}, wake{std::move(notify)}
    {
        if (!accept_stop.value || !stop.value) throw std::runtime_error{"cannot create pipe stop event"};
    }
    void run() noexcept
    {
        try
        {
            while (!closing)
            {
                auto connected = io(pipe.value, accept_stop.value, Operation::connect, nullptr, 0, Deadline::max());
                if (!connected)
                {
                    if (connected.error().kind == Failure::stopped) break;
                    // A client may open and close before ConnectNamedPipe begins.
                    if (connected.error().kind == Failure::disconnected) { DisconnectNamedPipe(pipe.value); continue; }
                    throw std::runtime_error{connected.error().message};
                }
                while (!stopped)
                {
                    auto bytes = read_frame(pipe.value, stop.value, std::chrono::steady_clock::now() + options.timeout, max_request_bytes);
                    if (!bytes || closing) break;
                    auto frame = std::make_shared<PendingFrame>();
                    frame->bytes = std::move(*bytes);
                    {
                        std::lock_guard lock{mutex};
                        if (queue.size() >= options.queued) break;
                        queue.push_back(frame);
                    }
                    wake();
                    const auto deadline = std::chrono::steady_clock::now() + options.timeout;
                    std::unique_lock lock{frame->mutex};
                    while (!frame->response && !stopped && remaining(deadline))
                        frame->ready.wait_for(lock, std::chrono::milliseconds{std::min<DWORD>(remaining(deadline), 20)});
                    if (!frame->response || stopped) break;
                    auto response = std::move(*frame->response);
                    lock.unlock();
                    if (!write_frame(pipe.value, stop.value, response, std::chrono::steady_clock::now() + options.timeout)) break;
                    // DisconnectNamedPipe discards unread bytes. Wait for peer EOF or its next frame.
                }
                DisconnectNamedPipe(pipe.value);
            }
        }
        catch (...) { std::lock_guard lock{mutex}; fatal = std::current_exception(); }
        { std::lock_guard lock{mutex}; done = true; }
        finished.notify_all();
        try { wake(); } catch (...) {}
    }
};
PipeServer::PipeServer(std::unique_ptr<Impl> impl) : impl_{std::move(impl)} {}
PipeServer::~PipeServer() { close(); }
PipeResult<std::unique_ptr<PipeServer>> PipeServer::listen(std::string_view name, std::function<void()> wake, const PipeOptions& options)
{
    if (!valid_endpoint(name) || !wake || options.queued == 0 || options.queued > 256 ||
        options.timeout.count() < 1 || options.timeout.count() > 60000)
        return std::unexpected(PipeError{Failure::invalid, "invalid pipe server options"});
    auto pipe = create_pipe(name);
    if (!pipe) return std::unexpected(pipe.error());
    auto server = std::unique_ptr<PipeServer>{new PipeServer{std::make_unique<Impl>(std::move(*pipe), options, std::move(wake))}};
    server->impl_->worker = std::jthread{[impl = server->impl_.get()] { impl->run(); }};
    return server;
}
std::optional<Incoming> PipeServer::pop()
{
    std::lock_guard lock{impl_->mutex};
    if (impl_->fatal) std::rethrow_exception(impl_->fatal);
    if (impl_->queue.empty()) return {};
    auto frame = std::move(impl_->queue.front());
    impl_->queue.pop_front();
    return Incoming{std::move(frame)};
}
void PipeServer::close(std::chrono::milliseconds grace) noexcept
{
    if (!impl_ || !impl_->worker.joinable()) return;
    impl_->closing = true;
    SetEvent(impl_->accept_stop.value);
    if (grace.count() > 0)
    {
        std::unique_lock lock{impl_->mutex};
        impl_->finished.wait_for(lock, grace, [&] { return impl_->done; });
    }
    impl_->stopped = true;
    SetEvent(impl_->stop.value);
    impl_->worker.join();
}
} // namespace dk::ipc

#include <dk/automation/IpcServer.hpp>
#include <limits>
#include <stdexcept>

namespace dk
{
namespace
{
bool fields(const Json& object, std::initializer_list<std::string_view> names)
{
    if (!object.is_object() || object.size() != names.size()) return false;
    for (const auto name : names) if (!object.contains(name)) return false;
    return true;
}
bool ticket(const Json& value)
{
    if (value.is_number_unsigned()) return value.get<std::uint64_t>() > 0 &&
        value.get<std::uint64_t>() <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    return value.is_number_integer() && value.get<std::int64_t>() > 0;
}
} // namespace
IpcSession::IpcSession(Runtime& runtime, IpcSessionOptions options) : runtime_{runtime}, options_{options}
{
    if (!options.retained || options.retained > 256 || options.bytes < ipc::max_request_bytes + ipc::max_response_bytes ||
        options.bytes > 32 * 1024 * 1024)
        throw std::invalid_argument{"invalid IPC ledger limits"};
    struct SessionTag;
    auto id = StableId<SessionTag>::generate();
    if (!id) throw std::runtime_error{id.error().message};
    id_ = id->to_string();
}
void IpcSession::evict(std::int64_t except)
{
    auto oldest = entries_.begin();
    if (oldest != entries_.end() && oldest->first == except) ++oldest;
    if (oldest == entries_.end()) throw std::logic_error{"IPC ledger cannot make room"};
    bytes_ -= oldest->second.request.size() + oldest->second.response.size();
    entries_.erase(oldest);
}
std::string IpcSession::wrap(Json response) const
{
    return Json{{"protocol", 1}, {"session", id_}, {"response", std::move(response)}}.dump();
}
std::string IpcSession::reject(Json id, int code, std::string message, bool unknown) const
{
    return wrap(rpc_error(std::move(id), code, std::move(message),
        Json{{"execution", unknown ? "unknown" : "not_executed"}}));
}
std::string IpcSession::handle(std::string_view frame)
{
    if (std::this_thread::get_id() != owner_) throw std::logic_error{"IPC dispatch requires the Runtime owner thread"};
    auto parsed = parse_command_json(frame);
    if (!parsed) return reject(nullptr, -32700, parsed.error().message);
    const auto& input = *parsed;
    if (!input.is_object() || !input.contains("protocol") || !input["protocol"].is_number_integer() || input["protocol"] != 1)
        return reject(nullptr, -32070, "Unsupported IPC envelope or protocol");
    if (input.contains("hello"))
    {
        if (!fields(input, {"protocol", "hello", "reserve"}) || input["hello"] != true || !input["reserve"].is_boolean())
            return reject(nullptr, -32070, "Invalid IPC hello");
        Json reserved = nullptr;
        if (input["reserve"].get<bool>())
        {
            if (next_ == std::numeric_limits<std::int64_t>::max()) return reject(nullptr, -32070, "IPC tickets exhausted");
            while (entries_.size() >= options_.retained) evict();
            entries_.emplace(next_, Entry{});
            reserved = next_++;
        }
        return Json{{"protocol", 1}, {"session", id_}, {"request_id", reserved},
            {"limits", {{"request_bytes", ipc::max_request_bytes}, {"response_bytes", ipc::max_response_bytes},
                        {"retained", options_.retained}, {"ledger_bytes", options_.bytes}}}}.dump();
    }
    if (!fields(input, {"protocol", "session", "request"}) || !input["session"].is_string())
        return reject(nullptr, -32070, "Invalid IPC request envelope");
    const auto& request = input["request"];
    if (!request.is_object() || !request.contains("id") || !ticket(request["id"]))
        return reject(nullptr, -32070, "IPC requires one request with a reserved positive integer id");
    const auto request_id = request["id"].get<std::int64_t>();
    if (input["session"] != id_) return reject(request_id, -32073, "Server session changed; request was not executed in this session");
    const auto found = entries_.find(request_id);
    if (found == entries_.end()) return reject(request_id, -32071, "Ticket expired or not reserved; original result may be unknown", true);
    auto canonical = request.dump();
    auto& entry = found->second;
    if (!entry.request.empty())
    {
        if (entry.request != canonical) return reject(request_id, -32072, "Ticket already bound to a different request");
        if (entry.response.empty()) return reject(request_id, -32074, "Request consumed; result unavailable", true);
        return entry.response;
    }
    // Make room before dispatch. The current ticket stays consumed even if dispatch/serialization throws.
    while (bytes_ + canonical.size() + ipc::max_response_bytes > options_.bytes) evict(request_id);
    entry.request = std::move(canonical);
    bytes_ += entry.request.size();
    try
    {
        ++dispatches_;
        auto outcome = dispatch_json_rpc(runtime_, request);
        auto response = wrap(outcome.response.value_or(rpc_error(request_id, -32600, "IPC notifications are unsupported")));
        if (response.size() > ipc::max_response_bytes)
            response = reject(request_id, -32075, "Response exceeds IPC limit; request may have executed", true);
        entry.response = std::move(response);
        bytes_ += entry.response.size();
    }
    catch (...)
    {
        // Persist the consumed request, never invoke it a second time.
        return reject(request_id, -32074, "Request consumed; result unavailable", true);
    }
    return entry.response;
}
Result<std::unique_ptr<IpcServer>> IpcServer::listen(Runtime& runtime, std::string_view name,
    const ipc::PipeOptions& pipe_options, IpcSessionOptions session_options)
{
    auto server = std::unique_ptr<IpcServer>{new IpcServer{runtime, session_options}};
    auto pipe = ipc::PipeServer::listen(name, [events = runtime.events()] { events->notify(); }, pipe_options);
    if (!pipe) return std::unexpected(Error{ErrorCode::io_error, pipe.error().message});
    server->pipe_ = std::move(*pipe);
    return server;
}
std::size_t IpcServer::pump(std::size_t budget)
{
    std::size_t processed = 0;
    while (processed < budget)
    {
        auto frame = pipe_->pop();
        if (!frame) break;
        frame->reply(session_.handle(frame->bytes()));
        ++processed;
    }
    return processed;
}
void IpcServer::close(std::chrono::milliseconds grace) noexcept { pipe_->close(grace); }
} // namespace dk

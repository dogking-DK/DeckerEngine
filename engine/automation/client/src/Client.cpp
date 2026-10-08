#include <dk/automation/Client.hpp>
#include <dk/core/StableId.hpp>
#include <limits>

namespace dk::ipc
{
Json ClientReply::json() const
{
    Json value{{"status", status}, {"execution", execution}};
    if (ticket) { value["session"] = ticket->session; value["request_id"] = ticket->request_id; }
    if (response) value["response"] = *response;
    if (hello) value["hello"] = *hello;
    if (!message.empty()) value["message"] = message;
    return value;
}
int ClientReply::exit_code() const
{
    if (status != "ok") return 3;
    return response && response->contains("error") ? 1 : 0;
}
namespace
{
ClientReply fail(ClientReply reply, const PipeError& error)
{
    switch (error.kind)
    {
    case Failure::timeout: reply.status = "timeout"; break;
    case Failure::disconnected: reply.status = "disconnected"; break;
    case Failure::invalid: reply.status = "invalid"; break;
    case Failure::stopped: reply.status = "stopped"; break;
    case Failure::system: reply.status = "system_error"; break;
    }
    reply.message = error.message;
    return reply;
}
bool positive_id(const Json& id)
{
    return id.is_number_integer() && (id.is_number_unsigned()
        ? id.get<std::uint64_t>() > 0 && id.get<std::uint64_t>() <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())
        : id.get<std::int64_t>() > 0);
}
bool rpc_response(const Json& response, std::int64_t id)
{
    if (!response.is_object() || response.size() != 3 || response.value("jsonrpc", Json{}) != "2.0" ||
        !response.contains("id") || !positive_id(response["id"]) || response["id"] != id ||
        response.contains("result") == response.contains("error")) return false;
    if (response.contains("error"))
    {
        const auto& error = response["error"];
        return error.is_object() && error.contains("code") && error["code"].is_number_integer() &&
            error.contains("message") && error["message"].is_string();
    }
    return true;
}
ClientReply exchange(const ClientOptions& options, std::optional<std::string_view> method, const Json& params)
{
    ClientReply reply;
    reply.ticket = options.retry;
    const auto invalid = [&](std::string message) { return fail(reply, {Failure::invalid, std::move(message)}); };
    struct SessionTag;
    if (!valid_endpoint(options.endpoint) || options.timeout.count() < 1 || options.timeout.count() > 60000)
        return invalid("Endpoint or timeout is invalid");
    if (options.retry && (!method || options.retry->request_id <= 0 || !StableId<SessionTag>::parse(options.retry->session)))
        return invalid("Retry requires a session UUID, positive request id and method");
    if (method && (method->empty() || method->size() > 256 || !params.is_object() || !validate_command_value(params, max_request_bytes)))
        return invalid("Method or parameters are invalid");
    const auto deadline = std::chrono::steady_clock::now() + options.timeout;
    auto connection = Connection::connect(options.endpoint, deadline);
    if (!connection) return fail(std::move(reply), connection.error());
    auto sent = connection->send(Json{{"protocol", 1}, {"hello", true}, {"reserve", method.has_value() && !options.retry}}.dump(), deadline);
    if (!sent) return fail(std::move(reply), sent.error());
    auto bytes = connection->receive(deadline);
    if (!bytes) return fail(std::move(reply), bytes.error());
    auto hello = parse_command_json(*bytes, max_response_bytes);
    if (!hello || !hello->is_object() || !hello->contains("protocol") || !(*hello)["protocol"].is_number_integer() || (*hello)["protocol"] != 1 ||
        !hello->contains("session") || !(*hello)["session"].is_string() ||
        !StableId<SessionTag>::parse((*hello)["session"].get<std::string>()) || !hello->contains("request_id") || !hello->contains("limits"))
        return invalid("Invalid IPC hello response");
    if (!method) { reply.hello = std::move(*hello); return reply; }
    const auto session = (*hello)["session"].get<std::string>();
    if (options.retry && options.retry->session != session) return invalid("Server session changed; old request was not resent");
    if (!options.retry)
    {
        if (!positive_id((*hello)["request_id"])) return invalid("Server did not reserve a valid request ticket");
        reply.ticket = RetryTicket{session, (*hello)["request_id"].get<std::int64_t>()};
    }
    const Json request{{"jsonrpc", "2.0"}, {"id", reply.ticket->request_id}, {"method", *method}, {"params", params}};
    const auto frame = Json{{"protocol", 1}, {"session", session}, {"request", request}}.dump();
    if (frame.size() > max_request_bytes) return invalid("Request exceeds IPC frame limit");
    // Any failure starting here must be reported as potentially executed, with a reusable ticket.
    reply.execution = "unknown";
    sent = connection->send(frame, deadline);
    if (!sent) return fail(std::move(reply), sent.error());
    bytes = connection->receive(deadline);
    if (!bytes) return fail(std::move(reply), bytes.error());
    auto result = parse_command_json(*bytes, max_response_bytes);
    if (!result || !result->is_object() || !result->contains("protocol") || !(*result)["protocol"].is_number_integer() || (*result)["protocol"] != 1 ||
        result->value("session", Json{}) != session || !result->contains("response"))
        return invalid("Invalid IPC response envelope");
    const auto& response = (*result)["response"];
    if (!rpc_response(response, reply.ticket->request_id)) return invalid("Invalid JSON-RPC response");
    reply.execution = "received";
    reply.response = response;
    return reply;
}
} // namespace
ClientReply discover(const ClientOptions& options) { return exchange(options, {}, Json::object()); }
ClientReply call(const ClientOptions& options, std::string_view method, const Json& params) { return exchange(options, method, params); }
} // namespace dk::ipc

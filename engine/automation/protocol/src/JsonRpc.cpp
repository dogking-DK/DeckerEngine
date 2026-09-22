#include <dk/automation/JsonRpc.hpp>
#include <limits>

namespace dk
{
Json rpc_error(Json id, int code, std::string message, std::optional<Json> data)
{
    Json error{{"code", code}, {"message", std::move(message)}};
    if (data)
        error["data"] = std::move(*data);
    return {{"jsonrpc", "2.0"}, {"id", std::move(id)}, {"error", std::move(error)}};
}
namespace
{
bool valid_id(const Json &id)
{
    if (id.is_null())
        return true;
    if (id.is_string())
        return id.get_ref<const std::string &>().size() <= 256;
    if (id.is_number_unsigned())
        return id.get<std::uint64_t>() <=
               static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    return id.is_number_integer();
}
RpcOutcome single(Runtime &runtime, const Json &request, bool auto_guard)
{
    if (!request.is_object() || !request.contains("jsonrpc") || request["jsonrpc"] != "2.0" ||
        !request.contains("method") || !request["method"].is_string() ||
        (request.contains("id") && !valid_id(request["id"])))
        return {rpc_error(nullptr, -32600, "Invalid Request"), true};
    for (const auto &[key, value] : request.items())
    {
        (void)value;
        if (key != "jsonrpc" && key != "method" && key != "params" && key != "id")
            return {rpc_error(nullptr, -32600, "Unknown request member"), true};
    }
    const bool notification = !request.contains("id");
    const Json id = notification ? Json(nullptr) : request["id"];
    auto error = [&](int code, std::string message, std::optional<Json> data = {}) -> RpcOutcome
    {
        return {notification ? std::optional<Json>{}
                             : std::optional<Json>{rpc_error(id, code, std::move(message), std::move(data))},
                true};
    };
    const auto method = request["method"].get<std::string>();
    if (!runtime.has_command(method))
        return error(-32601, "Method not found");
    const auto params = request.value("params", Json::object());
    if (!params.is_object())
        return error(-32602, "Named object parameters required");
    auto execution = runtime.dispatch(method, params, auto_guard);
    if (!execution || !execution->result)
    {
        const auto &e = execution ? execution->result.error() : execution.error();
        const int code =
            e.code == ErrorCode::invalid_argument
                ? -32602
                : (e.code == ErrorCode::internal_error ? -32603 : -32000 - static_cast<int>(e.code));
        Json data{{"engine_code", static_cast<unsigned>(e.code)},
                  {"engine_name", error_code_name(e.code)},
                  {"context", e.context}};
        if (execution)
        {
            data["task_id"] = execution->task_id.to_string();
            data["status"] = "failed";
        }
        return error(code, e.message, std::move(data));
    }
    if (notification)
        return {{}, false};
    return {Json{{"jsonrpc", "2.0"},
                 {"id", id},
                 {"result",
                  {{"value", std::move(*execution->result)},
                   {"task_id", execution->task_id.to_string()},
                   {"status", "succeeded"}}}},
            false};
}
} // namespace
RpcOutcome dispatch_json_rpc(Runtime &runtime, const Json &request, bool auto_guard)
{
    const auto valid = validate_command_value(request, 1024 * 1024);
    if (!valid)
        return {rpc_error(nullptr, -32600, valid.error().message), true};
    if (!request.is_array())
        return single(runtime, request, auto_guard);
    if (request.empty() || request.size() > 128)
        return {rpc_error(nullptr, -32600, "Batch requires 1-128 requests"), true};
    Json responses = Json::array();
    bool failed = false;
    for (const auto &item : request)
    {
        auto result = single(runtime, item, auto_guard);
        failed = failed || result.failed;
        if (result.response)
            responses.push_back(std::move(*result.response));
    }
    return {responses.empty() ? std::optional<Json>{} : std::optional<Json>{std::move(responses)}, failed};
}
RpcOutcome dispatch_json_line(Runtime &runtime, std::string_view line, bool auto_guard)
{
    auto parsed = parse_command_json(line);
    if (!parsed)
        return {rpc_error(nullptr, -32700, parsed.error().message), true};
    return dispatch_json_rpc(runtime, *parsed, auto_guard);
}
} // namespace dk

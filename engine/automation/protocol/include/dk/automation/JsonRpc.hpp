#pragma once
#include <dk/commands/CommandRegistry.hpp>
#include <functional>

namespace dk
{
struct RpcOutcome
{
    std::optional<Json> response;
    bool failed = false;
};
[[nodiscard]] Json rpc_error(Json id, int code, std::string message, std::optional<Json> data = {});
struct RpcCall { std::optional<std::string> task_id; Result<Json> result; };
struct RpcEndpoint {
    std::function<bool(std::string_view)> has_command;
    std::function<RpcCall(std::string_view,const Json&)> dispatch;
};
[[nodiscard]] RpcOutcome dispatch_json_rpc(const RpcEndpoint&, const Json&);
[[nodiscard]] RpcOutcome dispatch_json_line(const RpcEndpoint&, std::string_view);
} // namespace dk

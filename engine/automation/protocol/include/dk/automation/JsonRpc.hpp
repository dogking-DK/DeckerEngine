#pragma once
#include <dk/runtime/Runtime.hpp>

namespace dk
{
struct RpcOutcome
{
    std::optional<Json> response;
    bool failed = false;
};
[[nodiscard]] Json rpc_error(Json id, int code, std::string message, std::optional<Json> data = {});
[[nodiscard]] RpcOutcome dispatch_json_rpc(Runtime &runtime, const Json &request, bool auto_guard = false);
[[nodiscard]] RpcOutcome dispatch_json_line(Runtime &runtime, std::string_view line, bool auto_guard = false);
} // namespace dk

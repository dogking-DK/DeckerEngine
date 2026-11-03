#pragma once
#include <dk/editor/Console.hpp>

namespace dk::editor::detail {
// The production dispatch order also permits fault injection at the refresh
// boundary in CPU tests, without exposing a Runtime/Workspace test switch.
template<class Call, class Stopping, class Refresh>
Result<Json> dispatch_console(const ConsoleCommand& command, bool pending,
    Call&& call, Stopping&& stopping, Refresh&& refresh) {
    const auto& method = command.method;
    const auto& params = command.params;
    if (method == "jobs.wait" && params.contains("timeout_ms") &&
        (!params["timeout_ms"].is_number_integer() || params["timeout_ms"] != 0))
        return std::unexpected(Error{ErrorCode::invalid_argument,"Console jobs.wait requires integer timeout_ms=0; use jobs.get"});
    auto descriptor = call("commands.describe", Json{{"name",method}});
    if (!descriptor) return std::unexpected(descriptor.error());
    if (pending && (*descriptor)["effect"] != "query")
        return std::unexpected(Error{ErrorCode::invalid_state,"Apply or Revert the pending draft first."});
    auto result = call(method, params);
    if (!result) return result;
    if (!stopping()) {
        auto refreshed = refresh();
        if (!refreshed) {
            auto error = refreshed.error().with_context("console.executed_refresh_failed");
            error.message = "Command executed, but view refresh failed; query state before retrying. " + error.message;
            return std::unexpected(std::move(error));
        }
    }
    return result;
}
} // namespace dk::editor::detail

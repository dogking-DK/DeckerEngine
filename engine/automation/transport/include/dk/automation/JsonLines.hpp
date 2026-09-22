#pragma once
#include <dk/automation/JsonRpc.hpp>
#include <iosfwd>

namespace dk
{
// Exit 0 success, 1 recoverable batch errors, 3 transport failure. May throw on resource exhaustion.
[[nodiscard]] int run_json_lines(Runtime &runtime, std::istream &input, std::ostream &output,
                                 std::ostream &diagnostics, bool auto_guard = false);
} // namespace dk

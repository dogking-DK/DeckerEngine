#pragma once
#include <dk/automation/JsonRpc.hpp>
#include <iosfwd>

namespace dk
{
// Exit 0 success/stdio EOF, 1 batch errors, 2 incompatible options, 3 transport failure.
// May throw on resource exhaustion. Persistent mode requires explicit guards.
[[nodiscard]] int run_json_lines(Runtime &runtime, std::istream &input, std::ostream &output,
                                 std::ostream &diagnostics, bool auto_guard = false, bool persistent = false);
} // namespace dk

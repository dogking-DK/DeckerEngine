#pragma once
#include <dk/commands/Schema.hpp>
#include <cstddef>
#include <string>
#include <string_view>

namespace dk::editor {
inline constexpr std::size_t console_input_limit = 8192;
struct ConsoleCommand {
    std::string method;
    Json params;
};
// Pure, bounded single-line parsing. No Runtime access or implicit guard.
// Errors include console.syntax/console.limit and a zero-based UTF-8 byte offset.
[[nodiscard]] Result<ConsoleCommand> parse_console_command(std::string_view text);
} // namespace dk::editor

#pragma once
#include <dk/commands/CommandRegistry.hpp>

namespace dk::editor::detail {
struct ConsoleEditState {
    std::string text;
    std::size_t cursor = 0, selection_start = 0, selection_end = 0;
    std::uint64_t revision = 0;
    bool operator==(const ConsoleEditState&) const = default;
};
struct ConsoleCandidate {
    std::string label, insert, description;
};
struct ConsoleAssistance {
    ConsoleEditState source;
    CommandCatalogVersion catalog;
    std::size_t begin = 0, end = 0;
    std::vector<ConsoleCandidate> candidates;
    std::string hint;
    bool truncated = false;
};
// Pure metadata readers. No Runtime, task, filesystem or widget access.
[[nodiscard]] ConsoleAssistance assist_console(const CommandCatalogSnapshot&, const ConsoleEditState&);
[[nodiscard]] Result<ConsoleEditState> accept_console_candidate(const ConsoleAssistance&, std::size_t index,
    const ConsoleEditState& current, CommandCatalogVersion);
}

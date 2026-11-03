#pragma once
#include <dk/commands/CommandRegistry.hpp>
#include <dk/editor/Workspace.hpp>
#include "ConsoleStructure.hpp"

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
    ConsoleReferenceVersion references;
    std::string provider, kind_filter, search;
    std::vector<ConsoleDiagnostic> diagnostics;
    bool loading = false;
    std::uint64_t request_id = 0;
};
// Pure metadata readers. No Runtime, task, filesystem or widget access.
[[nodiscard]] ConsoleAssistance assist_console(const CommandCatalogSnapshot&, const ConsoleEditState&, ConsoleReferenceVersion = {});
void add_console_reference_page(ConsoleAssistance&, const ConsoleReferencePage&);
[[nodiscard]] Result<ConsoleEditState> accept_console_candidate(const ConsoleAssistance&, std::size_t index,
    const ConsoleEditState& current, CommandCatalogVersion, ConsoleReferenceVersion = {});
}

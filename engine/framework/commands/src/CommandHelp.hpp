#pragma once
#include <dk/commands/Schema.hpp>
#include <set>

namespace dk::detail {
Result<void> validate_command_help(const Json& parameters, const Json& help,
    const std::set<std::string, std::less<>>& providers);
}

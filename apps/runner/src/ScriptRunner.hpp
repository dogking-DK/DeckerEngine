#pragma once

#include <dk/scripting/Luau.hpp>
#include <filesystem>
#include <set>

namespace dk::runner {
struct ScriptOptions {
    LuauOptions execution;
    std::set<std::filesystem::path> specified;
};
// Called only for otherwise unknown CLI switches. Returns false on invalid input.
bool parse_script_option(const std::filesystem::path& flag, const std::filesystem::path& value, ScriptOptions& options);
int run_script(Runtime& runtime, const std::filesystem::path& file, const ScriptOptions& options);
} // namespace dk::runner

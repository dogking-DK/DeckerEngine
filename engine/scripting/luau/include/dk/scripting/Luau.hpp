#pragma once

#include <dk/core/Result.hpp>
#include <string_view>

namespace dk {
class Runtime;

// Synchronous, owner-thread only. Each call owns a fresh VM. Completed commands
// survive a later script error; use scene.transaction for atomic memory edits.
// M9.1 has no execution budget/cancellation: run trusted offline source only.
[[nodiscard]] Result<void> run_luau(Runtime& runtime, std::string_view source,
                                    std::string_view chunk_name = "scene.luau");
} // namespace dk

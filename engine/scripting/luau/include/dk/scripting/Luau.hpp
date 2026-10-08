#pragma once

#include <dk/core/Result.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <stop_token>
#include <string_view>

namespace dk {
class Runtime;

enum class LuauAccess { query, edit, project };
struct LuauLimits {
    std::chrono::milliseconds timeout{5000};
    std::uint64_t max_interrupts = 1000000;
    std::uint64_t max_commands = 10000;
    std::size_t max_vm_bytes = 64 * 1024 * 1024;
    std::size_t max_source_bytes = 1024 * 1024;
};
struct LuauOptions {
    LuauLimits limits;
    std::stop_token stop;
    LuauAccess access = LuauAccess::project;
};

// Synchronous, owner-thread only. Each call owns a fresh VM. Completed commands
// survive a later script error; use scene.transaction for atomic memory edits.
// Cancellation/limits are cooperative at VM safepoints and command boundaries;
// compilation/native calls cannot be preempted. This is not a security sandbox.
// A terminal Error uses invalid_state and context[0] = "luau.<reason>".
[[nodiscard]] Result<void> run_luau(Runtime& runtime, std::string_view source,
                                    std::string_view chunk_name = "scene.luau",
                                    const LuauOptions& options = {});
} // namespace dk

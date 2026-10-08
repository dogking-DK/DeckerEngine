#include "ScriptRunner.hpp"
#include <array>
#include <charconv>
#include <fstream>
#include <iostream>
#include <mutex>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#endif

namespace dk::runner {
namespace {
#ifdef _WIN32
std::mutex control_mutex;
std::stop_source* active_source = nullptr;
BOOL WINAPI control_handler(DWORD event) {
    if (event != CTRL_C_EVENT && event != CTRL_BREAK_EVENT) return FALSE;
    std::lock_guard lock(control_mutex);
    if (!active_source) return FALSE;
    active_source->request_stop();
    return TRUE;
}
struct ConsoleCancellation {
    std::stop_source source;
    bool registered = false;
    ConsoleCancellation() {
        std::lock_guard lock(control_mutex);
        active_source = &source;
        registered = SetConsoleCtrlHandler(control_handler, TRUE) != FALSE;
        if (!registered) active_source = nullptr;
    }
    ~ConsoleCancellation() {
        if (registered) SetConsoleCtrlHandler(control_handler, FALSE);
        // A callback already running finishes before source can be destroyed.
        std::lock_guard lock(control_mutex);
        active_source = nullptr;
    }
};
#endif
}

bool parse_script_option(const std::filesystem::path& flag, const std::filesystem::path& value, ScriptOptions& options) {
    if (!options.specified.insert(flag).second) return false;
    if (flag == "--script-access") {
        if (value == "query") options.execution.access = LuauAccess::query;
        else if (value == "edit") options.execution.access = LuauAccess::edit;
        else if (value == "project") options.execution.access = LuauAccess::project;
        else return false;
        return true;
    }
    const auto utf8 = value.u8string();
    const std::string_view text(reinterpret_cast<const char*>(utf8.data()), utf8.size());
    std::uint64_t number{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), number);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || number == 0) return false;
    if (flag == "--script-timeout-ms" && number <= 600000)
        options.execution.limits.timeout = std::chrono::milliseconds{number};
    else if (flag == "--script-max-interrupts" && number <= 1000000000)
        options.execution.limits.max_interrupts = number;
    else if (flag == "--script-max-commands" && number <= 100000)
        options.execution.limits.max_commands = number;
    else if (flag == "--script-memory-mib" && number <= 256)
        options.execution.limits.max_vm_bytes = static_cast<std::size_t>(number) * 1024 * 1024;
    else return false;
    return true;
}

int run_script(Runtime& runtime, const std::filesystem::path& file, const ScriptOptions& options) {
    if (file.extension() != ".luau") {
        std::cerr << "Script input must be a .luau source file.\n";
        return 2;
    }
    auto execution = options.execution;
#ifdef _WIN32
    ConsoleCancellation cancellation;
    if (!cancellation.registered) { std::cerr << "Cannot install script console cancellation handler.\n"; return 3; }
    execution.stop = cancellation.source.get_token();
#endif
    std::ifstream input(file, std::ios::binary);
    if (!input) { std::cerr << "Cannot open script file.\n"; return 2; }
    std::string source;
    std::array<char, 4096> buffer{};
    while (input.read(buffer.data(), buffer.size()) || input.gcount() != 0) {
        const auto size = static_cast<std::size_t>(input.gcount());
        if (size > execution.limits.max_source_bytes - source.size()) {
            std::cerr << "Luau source exceeds byte limit.\n";
            return 2;
        }
        source.append(buffer.data(), size);
    }
    if (input.bad()) { std::cerr << "Cannot read script file.\n"; return 2; }
    const auto path = file.u8string();
    const auto result = run_luau(runtime, source,
        std::string_view(reinterpret_cast<const char*>(path.data()), path.size()), execution);
    if (result) return 0;
    std::cerr << result.error().message << '\n';
    return !result.error().context.empty() && result.error().context.front() == "luau.cancelled" ? 130 : 2;
}
} // namespace dk::runner

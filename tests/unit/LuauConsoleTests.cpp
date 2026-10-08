#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include "SceneTestFiles.hpp"
#include <dk/runtime/Runtime.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <fstream>
#include <thread>

using namespace dk;
using namespace std::chrono_literals;
namespace {
struct Handle {
    HANDLE value = INVALID_HANDLE_VALUE;
    explicit Handle(HANDLE handle) : value(handle) {}
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};
struct Child {
    HANDLE process = nullptr;
    DWORD id = 0;
    ~Child() {
        if (!process) return;
        if (WaitForSingleObject(process, 0) == WAIT_TIMEOUT) {
            TerminateProcess(process, 99);
            WaitForSingleObject(process, 3000);
        }
        CloseHandle(process);
    }
};
BOOL WINAPI ignore_control(DWORD) { return TRUE; }
struct PrivateConsole {
    explicit PrivateConsole(DWORD child) {
        // Detach only this disposable CTest process; attach only our own child's
        // hidden console. Never send an event to the user's/CTest parent's console.
        FreeConsole();
        REQUIRE(AttachConsole(child));
        REQUIRE(SetConsoleCtrlHandler(ignore_control, TRUE));
    }
    ~PrivateConsole() { FreeConsole(); }
};
}

TEST_CASE("Luau runner console cancellation exits protected loop with saved state") {
    const auto signal = GENERATE(CTRL_C_EVENT, CTRL_BREAK_EVENT);
    INFO("Console event " << signal);
    SceneTestFiles files;
    files.write("cancel.luau", R"(
local function call(name, p)
    local r = dk.command(name, p)
    if not r.ok then error(r.error.message) end
    return r.value
end
local s = call("scene.new")
local function guard() return {document_id=s.document_id, revision=s.revision} end
s = call("entity.create", {guard=guard()}).state
s = call("scene.save", {guard=guard()})
call("project.save", {guard=guard(), manifest="project.json"})
while true do pcall(function() while true do end end) end
)");
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    Handle output(CreateFileW((files.root / "stdout.txt").c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                              &security, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    Handle errors(CreateFileW((files.root / "stderr.txt").c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                              &security, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    Handle input(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    REQUIRE(output.value != INVALID_HANDLE_VALUE);
    REQUIRE(errors.value != INVALID_HANDLE_VALUE);
    REQUIRE(input.value != INVALID_HANDLE_VALUE);
    const auto runner = path_from_utf8(DK_LUAU_RUNNER);
    REQUIRE(runner);
    auto arguments = L"\"" + runner->wstring() + L"\" --project-root \"" + files.root.wstring() +
        L"\" --script \"" + (files.root / "cancel.luau").wstring() +
        L"\" --script-timeout-ms 60000 --script-max-interrupts 1000000000";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    startup.hStdOutput = output.value;
    startup.hStdError = errors.value;
    startup.hStdInput = input.value;
    PROCESS_INFORMATION process{};
    REQUIRE(CreateProcessW(runner->c_str(), arguments.data(), nullptr, nullptr, TRUE,
                           CREATE_NEW_CONSOLE, nullptr, nullptr, &startup, &process));
    Child child;
    child.process = process.hProcess;
    child.id = process.dwProcessId;
    CloseHandle(process.hThread);
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!std::filesystem::exists(files.root / "project.json") &&
           WaitForSingleObject(child.process, 0) == WAIT_TIMEOUT && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(2ms);
    REQUIRE(std::filesystem::exists(files.root / "project.json"));
    {
        PrivateConsole console(child.id);
        // Group 0 here contains only our child and this disposable test process.
        REQUIRE(GenerateConsoleCtrlEvent(signal, 0));
        REQUIRE(WaitForSingleObject(child.process, 5000) == WAIT_OBJECT_0);
    }
    DWORD code = 0;
    REQUIRE(GetExitCodeProcess(child.process, &code));
    REQUIRE(code == 130);
    std::ifstream diagnostics(files.root / "stderr.txt", std::ios::binary);
    const std::string text{std::istreambuf_iterator<char>{diagnostics}, {}};
    REQUIRE(text.find("luau.cancelled") != std::string::npos);
    REQUIRE(std::filesystem::file_size(files.root / "stdout.txt") == 0);
    auto restored = Runtime::create(files.root);
    REQUIRE(restored);
    auto loaded = (*restored)->dispatch("scene.load", {{"manifest", "project.json"}});
    REQUIRE(loaded);
    REQUIRE(loaded->result);
    REQUIRE(loaded->result->at("entity_count") == 1);
    REQUIRE(loaded->result->at("dirty") == false);
}

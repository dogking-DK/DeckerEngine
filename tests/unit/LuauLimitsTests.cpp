#include "SceneTestFiles.hpp"
#include <dk/runtime/Runtime.hpp>
#include <dk/scripting/Luau.hpp>
#include <future>
#include <thread>

using namespace dk;
using namespace std::chrono_literals;
namespace {
struct LimitedScript {
    SceneTestFiles files;
    std::unique_ptr<Runtime> runtime;
    LimitedScript() {
        auto created = Runtime::create(files.root);
        REQUIRE(created);
        runtime = std::move(*created);
    }
    Result<void> run(std::string_view source, const LuauOptions& options = {}) {
        return run_luau(*runtime, source, "limits.luau", options);
    }
    Json call(std::string_view method, Json parameters = Json::object()) {
        auto result = runtime->dispatch(method, parameters);
        REQUIRE(result);
        REQUIRE(result->result);
        return *result->result;
    }
};
void require_stop(const Result<void>& result, std::string_view reason) {
    REQUIRE_FALSE(result);
    INFO(result.error().message);
    REQUIRE(result.error().code == ErrorCode::invalid_state);
    REQUIRE_FALSE(result.error().context.empty());
    REQUIRE(result.error().context.front() == reason);
}
constexpr auto save_then_loop = R"(
local state = dk.command("scene.new").value
assert(dk.command("scene.save", {guard = {document_id = state.document_id, revision = state.revision}}).ok)
while true do end
)";
bool wait_for_file(const std::filesystem::path& path, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (std::filesystem::exists(path)) return true;
        std::this_thread::sleep_for(1ms);
    }
    return false;
}
}

TEST_CASE("Luau interrupt budget terminates protected loops coroutines and metamethods") {
    LimitedScript s;
    LuauOptions options;
    options.limits.max_interrupts = 1000;
    for (const auto* source : {
        "while true do end",
        "while true do pcall(function() while true do end end) end",
        "xpcall(function() error('x') end, function() while true do end end)",
        "coroutine.resume(coroutine.create(function() while true do end end))",
        "coroutine.wrap(function() while true do end end)()",
        "pcall(function() tostring(setmetatable({}, {__tostring=function() while true do end end})) end)",
        "table.sort({3,2,1}, function() while true do pcall(function() while true do end end) end end)"
    }) {
        INFO(source);
        const auto started = std::chrono::steady_clock::now();
        require_stop(s.run(source, options), "luau.interrupt_limit");
        REQUIRE(std::chrono::steady_clock::now() - started < 2s);
        REQUIRE(s.run("assert(dk.command('commands.list').ok)"));
    }
}

TEST_CASE("Luau elapsed deadline terminates a loop and rejects invalid budgets before edits") {
    LimitedScript s;
    LuauOptions options;
    options.limits.timeout = 20ms;
    options.limits.max_interrupts = 1000000000;
    const auto started = std::chrono::steady_clock::now();
    require_stop(s.run("while true do end", options), "luau.timeout");
    REQUIRE(std::chrono::steady_clock::now() - started < 2s);
    for (int kind = 0; kind < 7; ++kind) {
        options = {};
        switch (kind) {
        case 0: options.limits.timeout = 0ms; break;
        case 1: options.limits.timeout = 600001ms; break;
        case 2: options.limits.max_interrupts = 0; break;
        case 3: options.limits.max_commands = 100001; break;
        case 4: options.limits.max_vm_bytes = 128 * 1024; break;
        case 5: options.limits.max_source_bytes = 0; break;
        case 6: options.access = static_cast<LuauAccess>(99); break;
        }
        const auto result = s.run("dk.command('scene.new')", options);
        REQUIRE_FALSE(result);
        REQUIRE(result.error().code == ErrorCode::invalid_argument);
    }
    const auto query = s.runtime->dispatch("scene.query", Json::object());
    REQUIRE(query);
    REQUIRE_FALSE(query->result);
    REQUIRE(s.run("assert(dk.command('scene.new').ok)"));
}

TEST_CASE("Luau command quota preserves committed work and cannot be caught") {
    LimitedScript s;
    LuauOptions options;
    options.limits.max_commands = 1;
    require_stop(s.run(R"(
local first = dk.command("scene.new")
pcall(function() dk.command("entity.create", {guard = {
    document_id = first.value.document_id, revision = first.value.revision}}) end)
error("command budget was bypassed")
)", options), "luau.command_limit");
    const auto state = s.call("scene.query")["state"];
    REQUIRE(state["entity_count"] == 0);
    REQUIRE(state["revision"] == 0);
    REQUIRE(s.call("history.status")["undo_count"] == 0);
    REQUIRE(s.run("assert(dk.command('scene.query').ok)", options));
    require_stop(s.run("dk.command('forbidden'); dk.command('scene.query')", options), "luau.command_limit");
    REQUIRE(s.call("scene.query")["state"] == state);
}

TEST_CASE("Luau VM allocation and source limits recover even when OOM is caught") {
    LimitedScript s;
    LuauOptions options;
    options.limits.max_vm_bytes = 1024 * 1024;
    for (int i = 0; i < 4; ++i) {
        require_stop(s.run(R"(
pcall(function() local huge = table.create(1000000) end)
dk.command("scene.new")
)", options), "luau.memory_limit");
        REQUIRE(s.run("assert(dk.command('commands.list').ok)", options));
    }
    options.limits.max_vm_bytes = 256 * 1024;
    const std::string large_literal = "local s = '" + std::string(350000, 'a') + "'";
    require_stop(s.run(large_literal, options), "luau.memory_limit");
    options = {};
    options.limits.max_source_bytes = 8;
    auto source = s.run("dk.command('scene.new')", options);
    REQUIRE_FALSE(source);
    REQUIRE(source.error().code == ErrorCode::invalid_argument);
    REQUIRE(s.run(R"(
local shared = string.rep("a", 600000)
local r = dk.command("scene.new", {name = shared, another = shared})
assert(not r.ok and r.task_id == dk.null and string.find(r.error.message, "byte limit"))
)"));
    const auto query = s.runtime->dispatch("scene.query", Json::object());
    REQUIRE(query);
    REQUIRE_FALSE(query->result);
    REQUIRE(s.run("assert(dk.command('scene.new').ok)"));
    REQUIRE_FALSE(s.run("coroutine.yield()"));
    REQUIRE(s.run("assert(dk.command('scene.query').ok)"));
}

TEST_CASE("Luau capability modes reject edits or file commands without side effects") {
    LimitedScript s;
    s.call("scene.new");
    LuauOptions options;
    options.access = LuauAccess::query;
    REQUIRE(s.run(R"(
for _, name in {"scene.new", "entity.create", "scene.transaction", "history.undo", "scene.load", "scene.save", "project.save"} do
    local r = dk.command(name)
    assert(not r.ok and r.error.name == "not_supported" and r.task_id == dk.null)
end
assert(dk.command("scene.query").ok)
)", options));
    options.access = LuauAccess::edit;
    REQUIRE(s.run(R"(
local s = dk.command("scene.query").value.state
assert(dk.command("entity.create", {guard = {document_id = s.document_id, revision = s.revision}}).ok)
for _, name in {"scene.load", "scene.save", "project.save"} do
    assert(dk.command(name).error.name == "not_supported")
end
)", options));
    REQUIRE(s.call("scene.query")["state"]["entity_count"] == 1);
    REQUIRE_FALSE(std::filesystem::exists(s.files.root / "scene.json"));
}

TEST_CASE("Luau cancellation before and during execution preserves state and permits recovery") {
    LimitedScript s;
    std::stop_source cancellation;
    LuauOptions options;
    options.stop = cancellation.get_token();
    cancellation.request_stop();
    require_stop(s.run("dk.command('scene.new')", options), "luau.cancelled");
    auto before = s.runtime->dispatch("scene.query", Json::object());
    REQUIRE(before);
    REQUIRE_FALSE(before->result);
    cancellation = std::stop_source{};
    options.stop = cancellation.get_token();
    options.limits.max_interrupts = 1000000000;
    bool saw_ready = false;
    std::jthread requester([&] {
        saw_ready = wait_for_file(s.files.root / "scene.json", 2s);
        cancellation.request_stop();
    });
    const auto started = std::chrono::steady_clock::now();
    const auto result = s.run(save_then_loop, options);
    requester.join();
    REQUIRE(saw_ready);
    require_stop(result, "luau.cancelled");
    REQUIRE(std::chrono::steady_clock::now() - started < 3s);
    REQUIRE(s.call("scene.query")["state"]["dirty"] == false);
    REQUIRE(s.run("local s=dk.command('scene.query').value.state; assert(dk.command('entity.create',{guard={document_id=s.document_id,revision=s.revision}}).ok)"));
    REQUIRE(s.call("scene.query")["state"]["entity_count"] == 1);
}

TEST_CASE("Luau owner shutdown requests stop and joins before Runtime destruction") {
    SceneTestFiles files;
    std::promise<Result<void>> completion;
    auto finished = completion.get_future();
    std::jthread owner([&](std::stop_token token) {
        auto runtime = Runtime::create(files.root);
        if (!runtime) { completion.set_value(std::unexpected(runtime.error())); return; }
        LuauOptions options;
        options.stop = token;
        options.limits.max_interrupts = 1000000000;
        auto result = run_luau(**runtime, save_then_loop, "shutdown.luau", options);
        runtime->reset();
        completion.set_value(std::move(result));
    });
    const bool ready = wait_for_file(files.root / "scene.json", 2s);
    const auto started = std::chrono::steady_clock::now();
    owner.request_stop();
    owner.join();
    REQUIRE(ready);
    REQUIRE(std::chrono::steady_clock::now() - started < 2s);
    require_stop(finished.get(), "luau.cancelled");
}

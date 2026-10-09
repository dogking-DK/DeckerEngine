#include "SceneTestFiles.hpp"
#include <dk/runtime/Runtime.hpp>
#include <dk/scripting/Luau.hpp>
#include <fstream>

using namespace dk;
namespace {
constexpr auto helpers = R"(
local function call(method, params)
    local r = dk.command(method, params)
    if not r.ok then error(r.error.name .. ": " .. r.error.message) end
    return r.value
end
local function guard()
    local s = call("scene.query").state
    return {document_id = s.document_id, revision = s.revision}
end
)";
struct Script {
    SceneTestFiles files;
    std::unique_ptr<Runtime> runtime;
    Script() {
        auto created = Runtime::create(files.root);
        REQUIRE(created);
        runtime = std::move(*created);
    }
    void run(std::string_view source) {
        const auto result = run_luau(*runtime, std::string(helpers) + std::string(source), "test.luau");
        if (!result) FAIL(result.error().message);
        REQUIRE(result);
    }
    Json call(std::string_view method, Json parameters = Json::object()) {
        auto execution = runtime->dispatch(method, parameters);
        REQUIRE(execution);
        REQUIRE(execution->result);
        return *execution->result;
    }
};
}

TEST_CASE("Luau simulation example executes exact ticks and capability modes protect controls") {
    Script s;
    std::ifstream input(DK_SIMULATION_EXAMPLE, std::ios::binary); REQUIRE(input);
    const std::string source{std::istreambuf_iterator<char>{input}, {}};
    LuauOptions edit; edit.access = LuauAccess::edit;
    REQUIRE(run_luau(*s.runtime, source, "fixed-step.luau", edit));
    REQUIRE(s.call("simulation.query")["mode"] == "edit");
    LuauOptions query; query.access = LuauAccess::query;
    REQUIRE(run_luau(*s.runtime, R"(
assert(dk.command("simulation.query").ok)
for _, method in {"simulation.start", "simulation.pause", "simulation.resume", "simulation.step", "simulation.stop", "simulation.export"} do
    local r = dk.command(method)
    assert(not r.ok and r.error.name == "not_supported" and r.task_id == dk.null)
end
)", "query-simulation.luau", query));
    REQUIRE(s.call("simulation.query")["mode"] == "edit");
}
TEST_CASE("Luau XPBD cloth example validates physics through controlled commands") {
    Script s;
    std::ifstream input(DK_XPBD_EXAMPLE, std::ios::binary); REQUIRE(input);
    const std::string source{std::istreambuf_iterator<char>{input}, {}};
    LuauOptions edit; edit.access = LuauAccess::edit;
    REQUIRE(run_luau(*s.runtime, R"(
local r = dk.command("simulation.export", {})
assert(not r.ok and r.error.name == "not_supported" and r.task_id == dk.null)
)", "export-denied.luau", edit));
    REQUIRE(run_luau(*s.runtime, source, "xpbd-cloth.luau", edit));
    s.call("simulation.start", {{"solver", "xpbd_cpu"}, {"paused", true}, {"guard", {
        {"document_id", s.call("scene.query")["state"]["document_id"]}, {"revision", 0}}}});
    LuauOptions query; query.access = LuauAccess::query;
    REQUIRE(run_luau(*s.runtime, R"(
local run = dk.command("simulation.query").value.run
local page = dk.command("simulation.particles", {run_id = run.run_id})
assert(page.ok and page.value.total == 64)
assert(dk.command("simulation.step", {run_id = run.run_id}).error.name == "not_supported")
)", "xpbd-query.luau", query));
}
TEST_CASE("Luau example creates edits saves and reloads the same service state") {
    Script s;
    std::ifstream input(DK_LUAU_EXAMPLE, std::ios::binary);
    REQUIRE(input);
    const std::string source{std::istreambuf_iterator<char>{input}, {}};
    s.run(source);
    const auto before = s.call("scene.query");
    REQUIRE(before["state"]["entity_count"] == 3);
    REQUIRE(before["state"]["dirty"] == false);
    int children = 0;
    for (const auto& entity : before["entities"]) {
        REQUIRE(entity["assets"].empty());
        if (!entity["parent"].is_null()) ++children;
        REQUIRE(entity["name"].get<std::string>().starts_with("实体 "));
    }
    REQUIRE(children == 2);
    auto reloaded = Runtime::create(s.files.root);
    REQUIRE(reloaded);
    auto load = (*reloaded)->dispatch("scene.load", {{"manifest", "project.json"}});
    REQUIRE(load);
    REQUIRE(load->result);
    auto query = (*reloaded)->dispatch("scene.query", Json::object());
    REQUIRE(query);
    REQUIRE(query->result);
    REQUIRE(query->result->at("entities") == before["entities"]);
    REQUIRE(query->result->at("state").at("revision") == before["state"]["revision"]);
    REQUIRE(query->result->at("state").at("document_id") != before["state"]["document_id"]);
}

TEST_CASE("Luau shares guard schema tasks transactions and undo semantics") {
    Script s;
    s.run(R"(
call("scene.new")
local original = guard()
local created = dk.command("entity.create", {guard = original})
assert(created.ok and type(created.task_id) == "string")
local id = created.value.created_id
assert(call("tasks.get", {id = created.task_id}).status == "succeeded")
local stale = dk.command("entity.set_name", {guard = original, id = id, name = "bad"})
assert(not stale.ok and stale.error.name == "conflict" and stale.error.code == 7)
assert(call("tasks.get", {id = stale.task_id}).status == "failed")
assert(not dk.command("entity.create").ok)
assert(not dk.command("entity.create", {guard = guard(), unknown = 1}).ok)
local before = call("scene.query").state
local failed = dk.command("scene.transaction", {guard = guard(), commands = {
    {method = "entity.set_name", params = {id = id, name = "temporary"}},
    {method = "entity.set_parent", params = {id = id, parent = id}},
}})
assert(not failed.ok and call("entity.get", {id = id}).name == "")
assert(call("scene.query").state.revision == before.revision)
assert(call("history.status").undo_count == 1)
local changed = call("scene.transaction", {guard = guard(), commands = {
    {method = "entity.set_name", params = {id = id, name = "changed"}},
    {method = "entity.set_parent", params = {id = id, parent = dk.null}},
}})
assert(#changed.created_ids == 2 and changed.created_ids[1] == dk.null and changed.created_ids[2] == dk.null)
assert(changed.state.revision == before.revision + 1)
call("history.undo", {guard = guard()})
assert(call("entity.get", {id = id}).name == "")
call("history.redo", {guard = guard()})
assert(call("entity.get", {id = id}).name == "changed")
)");
    const auto state = s.call("scene.query")["state"];
    auto direct = s.runtime->dispatch("entity.create", {{"guard", {{"document_id", state["document_id"]}, {"revision", 0}}}});
    REQUIRE(direct);
    REQUIRE_FALSE(direct->result);
    REQUIRE(direct->result.error().code == ErrorCode::conflict);
    REQUIRE(s.call("scene.query")["state"] == state);
}

TEST_CASE("Luau compile and runtime errors release VM and permit later work") {
    Script s;
    auto syntax = run_luau(*s.runtime, "dk.command('scene.new')\nlocal =", "broken.luau");
    REQUIRE_FALSE(syntax);
    REQUIRE(syntax.error().code == ErrorCode::invalid_argument);
    REQUIRE(syntax.error().message.find("broken.luau") != std::string::npos);
    auto empty = s.runtime->dispatch("scene.query", Json::object());
    REQUIRE(empty);
    REQUIRE_FALSE(empty->result);
    auto failure = run_luau(*s.runtime, "assert(dk.command('scene.new').ok)\nerror('intentional')", "failure.luau");
    REQUIRE_FALSE(failure);
    REQUIRE(failure.error().code == ErrorCode::invalid_state);
    REQUIRE(failure.error().message.find("failure.luau:2") != std::string::npos);
    for (int i = 0; i < 8; ++i) {
        const auto result = run_luau(*s.runtime, std::string(helpers) + R"(
call("entity.create", {guard = guard()})
leaked_global = 42
error({reason = "non-string"})
)");
        REQUIRE_FALSE(result);
        REQUIRE(result.error().message == "Luau raised a non-string error");
        s.run("assert(leaked_global == nil); assert(dk.command('scene.query').ok)");
    }
    REQUIRE(s.call("scene.query")["state"]["entity_count"] == 8);
    s.run("call('entity.create', {guard = guard()})");
    REQUIRE(s.call("scene.query")["state"]["entity_count"] == 9);
    REQUIRE_FALSE(run_luau(*s.runtime, std::string("\x1bLua\0", 5)));
    REQUIRE_FALSE(run_luau(*s.runtime, "", std::string_view("bad\0name", 8)));
}

TEST_CASE("Luau rejects ambiguous or unsupported values without scene effects") {
    Script s;
    s.run(R"(
call("scene.new")
local cycle = {}; cycle.self = cycle
local sparse = {[2] = "x"}
local mixed = {[1] = "x", named = "y"}
local marked = dk.array(); marked.named = true
local deep = {}; local p = deep
for i = 1, 66 do p.next = {}; p = p.next end
local invalid = {cycle, sparse, mixed, marked, deep, function() end,
    0/0, 1/0, 9007199254740992, -9007199254740992, newproxy(),
    setmetatable({}, {__index = function() error("metamethod must not run") end})}
for _, value in invalid do
    local r = dk.command("scene.new", {name = value})
    assert(not r.ok and r.error.name == "invalid_argument" and r.task_id == dk.null)
end
assert(not dk.command(12).ok)
assert(not dk.command("scene.query", {}, {}).ok)
assert(not dk.command("scene.query", dk.array()).ok)
assert(not dk.command("scene.query", {limit = 1.5}).ok)
assert(not dk.command("scene.new", {guard = guard(), name = "a\0b"}).ok)
assert(not dk.command("scene.new", {guard = guard(), name = "\255"}).ok)
assert(call("scene.query").state.entity_count == 0)
assert(call("scene.query").state.revision == 0)
local child = call("entity.create", {guard = guard()}).created_id
assert(not dk.command("entity.set_assets", {guard = guard(), id = child, assets = {}}).ok)
call("entity.set_assets", {guard = guard(), id = child, assets = dk.array()})
local entity = call("entity.get", {id = child})
assert(entity.parent == dk.null and #entity.assets == 0)
call("entity.set_assets", {guard = guard(), id = child, assets = entity.assets})
)");
}

TEST_CASE("Luau preserves full revision integers and discovery schemas") {
    Script s;
    s.run(R"(
call("scene.new")
call("scene.save", {guard = guard()})
call("project.save", {guard = guard(), manifest = "project.json"})
for _, item in call("commands.list") do
    local descriptor = call("commands.describe", {name = item.name})
    assert(descriptor.name == item.name)
end
assert(tostring(dk.integer("18446744073709551615")) == "18446744073709551615")
assert(tostring(dk.integer("-9223372036854775808")) == "-9223372036854775808")
assert(not pcall(dk.integer, "18446744073709551616"))
assert(not pcall(dk.integer, "-9223372036854775809"))
assert(not pcall(dk.integer, 1))
assert(not pcall(dk.integer, "1\0x"))
)");
    const auto path = s.files.root / "scene.json";
    Json scene;
    { std::ifstream input(path); input >> scene; }
    scene["revision"] = std::uint64_t{9007199254740993ULL};
    s.files.write("scene.json", scene.dump());
    s.run(R"(
local loaded = call("scene.load", {guard = guard(), manifest = "project.json"})
assert(tostring(loaded.revision) == "9007199254740993")
local created = call("entity.create", {guard = guard()})
assert(tostring(created.state.revision) == "9007199254740994")
local g = guard(); g.revision = dk.integer("18446744073709551615")
local r = dk.command("entity.create", {guard = g})
assert(not r.ok and r.error.name == "conflict")
)");
    REQUIRE(s.call("scene.query")["state"]["revision"] == std::uint64_t{9007199254740994ULL});
}

TEST_CASE("Luau exposes only scene commands and isolates builtin tables") {
    Script s;
    s.run(R"(
for _, method in {"runtime.shutdown", "jobs.wait", "assets.import", "render.capture", "future.command"} do
    local result = dk.command(method)
    assert(not result.ok and result.error.name == "not_supported" and result.task_id == dk.null)
end
assert(io == nil and package == nil and require == nil and loadstring == nil and print == nil)
assert(os.execute == nil and os.exit == nil and loadfile == nil and dofile == nil)
assert(not pcall(function() dk.command = false end))
assert(not pcall(function() table.insert = false end))
assert(not pcall(function() setmetatable(dk.array(), {}) end))
local co = coroutine.create(function() assert(dk.command("scene.new").ok) end)
assert(coroutine.resume(co))
)");
    REQUIRE_FALSE(s.runtime->stopping());
    s.run("assert(type(dk.command) == 'function'); assert(dk.command('scene.query').ok)");
}

#include "SceneTestFiles.hpp"
#include "ConsoleDispatch.hpp"
#include <dk/editor/Console.hpp>
#include <dk/editor/Workspace.hpp>
#include <algorithm>
#include <limits>

using namespace dk;
using namespace dk::editor;
namespace {
ConsoleCommand parsed(std::string_view text) {
    auto result = parse_console_command(text);
    INFO(text);
    if (!result) INFO(result.error().message);
    REQUIRE(result);
    return std::move(*result);
}
void rejected(std::string_view text, std::string_view category = "console.syntax") {
    const auto result = parse_console_command(text);
    INFO(text);
    REQUIRE_FALSE(result);
    CHECK(result.error().code == ErrorCode::invalid_argument);
    REQUIRE(result.error().context.size() == 2);
    CHECK(result.error().context[0] == category);
    CHECK(result.error().context[1].starts_with("byte="));
}
struct Fixture {
    SceneTestFiles files;
    std::unique_ptr<Workspace> model;
    Fixture() {
        std::filesystem::copy(DK_EDITOR_FIXTURE_DIR, files.root, std::filesystem::copy_options::recursive);
        model = Workspace::create(files.root).value();
        REQUIRE(model->open("project.json"));
    }
    Json guard() const {
        const auto& s = model->snapshot()->state;
        return {{"document_id",s.document_id.to_string()},{"revision",s.revision}};
    }
};
}

TEST_CASE("console named parameters preserve scalar types and whitespace") {
    auto query = parsed(" \tscene.query\t ");
    CHECK(query.method == "scene.query"); CHECK(query.params == Json::object());
    const auto c = parsed("scene.query limit = 20 offset=-1 f=1.0 exp=1e3 yes=true no=false nil=null name=render.capture");
    CHECK(c.params["limit"].is_number_integer()); CHECK(c.params["limit"] == 20);
    CHECK(c.params["offset"] == -1);
    CHECK(c.params["f"].is_number_float()); CHECK(c.params["exp"] == 1000.0);
    CHECK(c.params["yes"] == true); CHECK(c.params["no"] == false); CHECK(c.params["nil"].is_null());
    CHECK(c.params["name"] == "render.capture");
    const auto strings = parsed("test v=01 w=1e x=NaN y=Infinity z=+1");
    for (const auto& value : strings.params) CHECK(value.is_string());
    CHECK(parsed("a v=one").params != parsed("a v=two").params);
}

TEST_CASE("console quotes unicode paths and reserved characters are literal") {
    const auto c = parsed(R"(test name="你好 😀" quote="a\"b" line="a\nb" raw='C:\folder with spaces\file' plain=C:\folder\file empty='' number="123" boolean="true" reserved=';|&=[]{}' encoded="\u4f60\u597d")");
    CHECK(c.params["name"] == "你好 😀"); CHECK(c.params["quote"] == "a\"b");
    CHECK(c.params["line"] == "a\nb"); CHECK(c.params["raw"] == R"(C:\folder with spaces\file)");
    CHECK(c.params["plain"] == R"(C:\folder\file)"); CHECK(c.params["empty"] == "");
    CHECK(c.params["number"] == "123"); CHECK(c.params["boolean"] == "true");
    CHECK(c.params["reserved"] == ";|&=[]{}"); CHECK(c.params["encoded"] == "你好");
    CHECK(parsed("test v=$HOME x=$(literal)").params["v"] == "$HOME");
}

TEST_CASE("console nested JSON and legacy envelopes have identical values") {
    const auto c = parsed(R"(entity.set_transform id=example transform={"translation":[1, 2, 3],"text":"a } ] \" b","nested":[{},[],null,true]} guard={"revision":18446744073709551615} empty=[])");
    const auto legacy = Json{{"method",c.method},{"params",c.params}}.dump();
    const auto other = parsed(legacy);
    CHECK(other.method == c.method); CHECK(other.params == c.params);
    CHECK(other.params["empty"].is_array()); CHECK(other.params["empty"].empty());
    rejected(R"(test v={"x":1,"x":2})");
    rejected(R"(test v={"x":1,"\u0078":2})");
    rejected(R"({"method":"test","params":{},"method":"other"})");
    rejected(R"({"method":"test","params":{"x":{"y":1,"y":2}}})");
    CHECK(parsed(R"(test v=[{"x":1},{"x":2}])").params["v"].size() == 2);
}

TEST_CASE("console full integer precision and overflow are checked in every syntax") {
    for (const std::string token : {"-9223372036854775808", "9223372036854775807", "9223372036854775808", "18446744073709551615"}) {
        const auto value = parsed("test v=" + token).params["v"];
        REQUIRE(value.is_number_integer()); CHECK(value.dump() == token);
        CHECK(parsed("test v=[" + token + "]").params["v"][0] == value);
        CHECK(parsed("{\"method\":\"test\",\"params\":{\"v\":" + token + "}}").params["v"] == value);
    }
    for (const std::string token : {"-9223372036854775809", "18446744073709551616", "999999999999999999999999999999"}) {
        rejected("test v=" + token, "console.limit");
        rejected("test v=[{\"n\":" + token + "}]", "console.limit");
        rejected("{\"method\":\"test\",\"params\":{\"v\":" + token + "}}", "console.limit");
        CHECK(parsed("test v='" + token + "'").params["v"] == token);
    }
    rejected("test v=1e9999", "console.limit");
    rejected("test v=[1e9999]", "console.limit");
    CHECK(parsed("test v=1.8446744073709552e19").params["v"].is_number_float());
    CHECK(parsed("test v=-0").params["v"].is_number_integer());
}

TEST_CASE("console malformed lines never recover a valid prefix") {
    for (const auto* text : {"", " \t", "20", "scene.query 20", "scene.query limit", "scene.query limit=",
        "scene.query limit=1 limit=2", "test guard.revision=1", "test x='unclosed", "test x=\"unclosed",
        R"(test x="bad\q")", R"(test x="\uD800")", "test x={", "test x=[}", "test x={]", "test x=[1,]",
        "test x={a:1}", "test x={'a':1}", "test x=[01]", "test x=[NaN]", "test x=[]suffix=1",
        "test x=\"a\"suffix", "test x=a=b", "test x=1;scene.query", "test x=1 | scene.query", "test x=a&b",
        "test x=1 # comment", "scene.query\nscene.query", "scene.query\r", "scene.query\v", "scene.query\f",
        "{", "{}", "[]", "{\"method\":\"scene.query\"}", "{\"method\":\"test\",\"params\":[]}",
        "{\"method\":\"test\",\"params\":{},\"extra\":0}", "{\"method\":\"test\",\"params\":{}} tail"}) rejected(text);
    rejected(std::string{"test v=a\0b",10});
    rejected(std::string(97,'a'));
    CHECK(parsed(std::string(96,'a')).method.size() == 96);
}

TEST_CASE("console limits reject before unbounded DOM construction") {
    const std::string boundary = "test v=" + std::string(8192-7,'x');
    CHECK(parsed(boundary).params["v"].get<std::string>().size() == 8192-7);
    rejected(boundary + "x", "console.limit");
    // Full envelope root=0, params=1, first value=2, final scalar=64.
    const auto nested = [](std::size_t n) { return std::string(n,'[') + "0" + std::string(n,']'); };
    CHECK(parsed("test v=" + nested(62)).params["v"].is_array());
    rejected("test v=" + nested(63), "console.limit");
    CHECK(parsed("{\"method\":\"test\",\"params\":{\"v\":" + nested(62) + "}}").params["v"].is_array());
    rejected("{\"method\":\"test\",\"params\":{\"v\":" + nested(63) + "}}", "console.limit");
    rejected("test v=" + nested(2000), "console.limit");
}

TEST_CASE("console diagnostics locate unicode columns duplicates and JSON errors") {
    for (const std::string bytes : {std::string("\xc0\xaf"), std::string("\xed\xa0\x80"),
        std::string("\xf4\x90\x80\x80"), std::string("\xe4\xb8"), std::string("\x80"), std::string("\xe4x\x80")}) {
        const auto result = parse_console_command("test v=" + bytes);
        REQUIRE_FALSE(result); CHECK(result.error().message.find("Invalid UTF-8 at byte") != std::string::npos);
    }
    const std::string line = "test name='中文' x=";
    const auto missing = parse_console_command(line);
    REQUIRE_FALSE(missing);
    CHECK(missing.error().context[1] == "byte=" + std::to_string(line.size()));
    CHECK(missing.error().message.find("column 18") != std::string::npos);
    const auto duplicate = parse_console_command("test x=1 x=2");
    REQUIRE_FALSE(duplicate); CHECK(duplicate.error().context[1] == "byte=9");
    const auto unclosed = parse_console_command("test x=[1");
    REQUIRE_FALSE(unclosed); CHECK(unclosed.error().context[1] == "byte=7");
    const auto bad_json = parse_console_command("test x=[1,]");
    REQUIRE_FALSE(bad_json); CHECK(bad_json.error().context[1] == "byte=10");
    const auto duplicate_json = parse_console_command(R"(test x={"a":1,"a":2})");
    REQUIRE_FALSE(duplicate_json); CHECK(duplicate_json.error().context[1] == "byte=14");
}

TEST_CASE("workspace console text shares schemas discovery and JSON query results") {
    Fixture f;
    auto text = f.model->execute_console("scene.query limit=1 offset=0"); REQUIRE(text);
    auto json = f.model->execute_console(R"({"method":"scene.query","params":{"limit":1,"offset":0}})"); REQUIRE(json);
    CHECK(*text == *json);
    auto descriptor = f.model->execute_console("commands.describe name=entity.create"); REQUIRE(descriptor);
    CHECK((*descriptor)["effect"] == "memory_edit"); CHECK((*descriptor)["undoable"] == true);
    CHECK((*descriptor)["parameters"]["properties"].contains("guard"));
    auto list = f.model->execute_console("commands.list"); REQUIRE(list);
    CHECK(std::ranges::any_of(*list, [](const Json& d) { return d["name"] == "scene.query"; }));
    for (const auto* input : {"scene.query limit=1.0", "scene.query limit=01", "scene.query unknown=1"}) {
        auto r = f.model->execute_console(input); REQUIRE_FALSE(r); CHECK(r.error().code == ErrorCode::invalid_argument);
        CHECK(std::ranges::find(r.error().context,"console.syntax") == r.error().context.end());
    }
    auto unknown = f.model->execute_console("camera.orbit x=1"); REQUIRE_FALSE(unknown);
    CHECK(unknown.error().code == ErrorCode::not_found);
}

TEST_CASE("workspace console full-line failures preserve scene files history and tasks") {
    Fixture f;
    const auto before = f.model->snapshot()->state;
    const auto disk = read_file_bytes(f.files.root / "scene.json"); REQUIRE(disk);
    const auto tasks = f.model->execute_console("tasks.list"); REQUIRE(tasks);
    const auto guard = f.guard().dump();
    for (const auto& input : {"entity.create guard=" + guard + " trailing", "entity.create guard=" + guard + " x=[1,]",
        "scene.save guard=" + guard + " ; scene.query", "project.save guard=" + guard + " manifest='should-not-exist.json' extra="})
        REQUIRE_FALSE(f.model->execute_console(input));
    CHECK(f.model->snapshot()->state.revision == before.revision);
    CHECK(f.model->snapshot()->state.dirty == before.dirty);
    CHECK(f.model->history().undo_count == 0);
    CHECK(read_file_bytes(f.files.root / "scene.json").value() == *disk);
    CHECK_FALSE(std::filesystem::exists(f.files.root / "should-not-exist.json"));
    const auto after = f.model->execute_console("tasks.list"); REQUIRE(after);
    // Exclude queries caused by the two diagnostic calls themselves.
    for (std::size_t i = tasks->size(); i < after->size(); ++i) {
        const auto method = (*after)[i]["method"].get<std::string>();
        CHECK((method == "tasks.list" || method == "scene.query" || method == "history.status" ||
            method == "environment.query" || method == "commands.describe"));
    }
    const auto describes = [](const Json& t) { return std::ranges::count_if(t, [](const Json& j) { return j["method"] == "commands.describe"; }); };
    CHECK(describes(*after) == describes(*tasks) + 1);
}

TEST_CASE("workspace console text edits preserve explicit guards drafts and undo") {
    Fixture f;
    const auto before = f.model->snapshot()->state;
    const auto create = "entity.create guard=" + f.guard().dump();
    REQUIRE_FALSE(f.model->execute_console("entity.create"));
    REQUIRE(f.model->select(f.model->snapshot()->scene.entities().front().id));
    f.model->draft()->modified = true;
    REQUIRE_FALSE(f.model->execute_console(create)); REQUIRE(f.model->execute_console("scene.query"));
    REQUIRE(f.model->revert());
    f.model->environment_draft()->modified = true;
    REQUIRE_FALSE(f.model->execute_console(create)); REQUIRE(f.model->execute_console("history.status"));
    REQUIRE(f.model->revert_environment());
    auto created = f.model->execute_console(create); REQUIRE(created);
    CHECK(f.model->snapshot()->state.revision == before.revision + 1);
    CHECK(f.model->history().undo_count == 1);
    auto stale = f.model->execute_console(create); REQUIRE_FALSE(stale); CHECK(stale.error().code == ErrorCode::conflict);
    REQUIRE(f.model->execute_console("history.undo guard=" + f.guard().dump()));
    CHECK(f.model->snapshot()->scene.entities().size() == before.entity_count);
    REQUIRE(f.model->execute_console("history.redo guard=" + f.guard().dump()));
    CHECK(f.model->snapshot()->scene.entities().size() == before.entity_count + 1);
}

TEST_CASE("workspace console waits are zero only before dispatch") {
    Fixture f;
    for (const auto* timeout : {"1", "1000", "-1", "0.0", "false", "null", "\"0\""}) {
        auto r = f.model->execute_console(std::string("jobs.wait id=missing timeout_ms=") + timeout);
        REQUIRE_FALSE(r); CHECK(r.error().message.find("integer timeout_ms=0") != std::string::npos);
    }
    // The real registry validates id when omission or integer zero passes the front end.
    for (const auto* suffix : {"", " timeout_ms=0"}) {
        auto r = f.model->execute_console(std::string("jobs.wait id=missing") + suffix);
        REQUIRE_FALSE(r); CHECK(r.error().message.find("integer timeout_ms=0") == std::string::npos);
    }
    auto legacy = f.model->execute_console(R"({"method":"jobs.wait","params":{"timeout_ms":0.0}})");
    REQUIRE_FALSE(legacy); CHECK(legacy.error().message.find("integer timeout_ms=0") != std::string::npos);
}

TEST_CASE("console committed command survives injected refresh failure without retry") {
    SceneTestFiles files;
    auto runtime = Runtime::create(files.root).value();
    const auto call = [&](std::string_view method, const Json& params) -> Result<Json> {
        auto execution = runtime->dispatch(method, params);
        if (!execution) return std::unexpected(execution.error());
        return std::move(execution->result);
    };
    auto state = call("scene.new", Json::object()); REQUIRE(state);
    const auto command = parsed("entity.create guard=" + Json{{"document_id",(*state)["document_id"]},{"revision",(*state)["revision"]}}.dump());
    int refreshes = 0;
    auto result = dk::editor::detail::dispatch_console(command, false, call, [&] { return runtime->stopping(); }, [&]() -> Result<void> {
        ++refreshes;
        return std::unexpected(Error{ErrorCode::io_error,"Injected refresh failure",{"test.refresh"}});
    });
    REQUIRE_FALSE(result); CHECK(refreshes == 1); CHECK(result.error().code == ErrorCode::io_error);
    CHECK(result.error().context == std::vector<std::string>{"test.refresh","console.executed_refresh_failed"});
    CHECK(result.error().message.starts_with("Command executed, but view refresh failed"));
    auto after = call("scene.query", Json::object()); REQUIRE(after);
    CHECK((*after)["state"]["entity_count"] == 1); CHECK((*after)["state"]["revision"] == 1);
    auto tasks = call("tasks.list", Json::object()); REQUIRE(tasks);
    CHECK(std::ranges::count_if(*tasks, [](const Json& t) { return t["method"] == "entity.create"; }) == 1);
    auto failed = dk::editor::detail::dispatch_console(command, false, call, [&] { return false; }, [&]() -> Result<void> { ++refreshes; return {}; });
    REQUIRE_FALSE(failed); CHECK(failed.error().code == ErrorCode::conflict); CHECK(refreshes == 1);
    auto stopped = dk::editor::detail::dispatch_console(parsed("runtime.shutdown"), false, call,
        [&] { return runtime->stopping(); }, [&]() -> Result<void> { ++refreshes; return {}; });
    REQUIRE(stopped); CHECK(runtime->stopping()); CHECK(refreshes == 1);
}

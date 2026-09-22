#include <dk/commands/CommandRegistry.hpp>
#include <catch2/catch_test_macros.hpp>
#include <limits>

using namespace dk;
TEST_CASE("discovery is sorted and exposes executable schemas") {
    CommandRegistry commands;
    REQUIRE(commands.add({"test.echo", "Echo", schema::object({{"value", schema::string()}}, {"value"}), schema::string()}, [](const Json& p) -> Result<Json> { return p["value"]; }));
    const auto list = commands.execute("commands.list"); REQUIRE(list);
    REQUIRE((*list)[0]["name"] == "commands.describe");
    auto description = commands.execute("commands.describe", {{"name", "test.echo"}}); REQUIRE(description);
    REQUIRE(schema::validate((*description)["parameters"], {{"value", "hello"}}));
    REQUIRE(commands.execute("test.echo", {{"value", "hello"}}).value() == "hello");
    REQUIRE_FALSE(commands.execute("test.echo", {{"value", "hi"}, {"extra", 1}}));
    REQUIRE_FALSE(commands.execute("test.echo"));
    REQUIRE(commands.execute("missing").error().code == ErrorCode::not_found);
}
TEST_CASE("registration rejects duplicate names invalid schemas and handlers") {
    CommandRegistry c;
    auto handler = [](const Json&) -> Result<Json> { return Json(); };
    REQUIRE_FALSE(c.add({"commands.list", "duplicate", Json::object(), Json::object()}, handler));
    REQUIRE_FALSE(c.add({"1bad", "bad", Json::object(), Json::object()}, handler));
    REQUIRE_FALSE(c.add({"bad", "bad", {{"typo", 1}}, Json::object()}, handler));
    REQUIRE_FALSE(c.add({"bad", "bad", Json::object(), Json::object()}, {}));
    REQUIRE_FALSE(schema::check({{"type", "integer"}}));
    REQUIRE_FALSE(schema::check({{"type", "oops"}}));
    REQUIRE_FALSE(schema::check({{"type", "array"}, {"minItems", 3}, {"maxItems", 2}}));
    REQUIRE_FALSE(schema::check(schema::object({}, {"missing"})));
}
TEST_CASE("nested schemas enforce bounds integer tokens nullable and unicode") {
    auto s = schema::object({{"values", schema::array(schema::nullable(schema::integer()), 1, 2)}, {"name", schema::string(1, 2)}}, {"values", "name"});
    REQUIRE(schema::validate(s, {{"values", {1, nullptr}}, {"name", "中文"}}));
    REQUIRE_FALSE(schema::validate(s, {{"values", {1.0}}, {"name", "中"}}));
    REQUIRE_FALSE(schema::validate(s, {{"values", {1}}, {"name", "中文长"}}));
    REQUIRE_FALSE(schema::validate(s, {{"values", Json::array()}, {"name", "a"}}));
    auto n = schema::number(); n["minimum"] = 1; n["maximum"] = 2;
    REQUIRE(schema::validate(n, 1.5)); REQUIRE_FALSE(schema::validate(n, 3));
    n["enum"] = {1, 2}; REQUIRE_FALSE(schema::validate(n, 1.5));
    REQUIRE_FALSE(schema::validate(schema::number(), std::numeric_limits<double>::infinity()));
    REQUIRE_FALSE(schema::validate(schema::string(), std::string(1, static_cast<char>(0xff))));
}
TEST_CASE("command errors preserve business context and contain handler faults") {
    CommandRegistry c;
    REQUIRE(c.add({"fail.business", "Error", schema::object(), Json::object()}, [](const Json&) -> Result<Json> { return std::unexpected(Error{ErrorCode::io_error, "disk", {"inner"}}); }));
    REQUIRE(c.add({"fail.throw", "Throw", schema::object(), Json::object()}, [](const Json&) -> Result<Json> { throw std::runtime_error("fault"); }));
    REQUIRE(c.add({"fail.result", "Bad result", schema::object(), schema::integer()}, [](const Json&) -> Result<Json> { return "wrong"; }));
    REQUIRE(c.execute("fail.business").error().context == std::vector<std::string>{"inner", "fail.business"});
    REQUIRE(c.execute("fail.throw").error().code == ErrorCode::internal_error);
    REQUIRE(c.execute("fail.result").error().code == ErrorCode::internal_error);
}
TEST_CASE("strict command JSON rejects duplicate keys malformed input and limits") {
    REQUIRE(parse_command_json(R"({"a":[{"b":1},{"b":2}]})"));
    for (const auto* text : {R"({"a":1,"a":2})", R"({"x":{"a":1,"a":2}})", "{} trailing", "NaN", "1e999"}) REQUIRE_FALSE(parse_command_json(text));
    REQUIRE_FALSE(parse_command_json(std::string(65, '[') + "0" + std::string(65, ']')));
    REQUIRE_FALSE(parse_command_json(std::string(1024 * 1024 + 1, ' ')));
    REQUIRE_FALSE(validate_command_value(Json::array({1, 2}), 2));
}

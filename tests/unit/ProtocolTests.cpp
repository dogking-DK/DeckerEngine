#include "SceneTestFiles.hpp"
#include <dk/automation/JsonLines.hpp>
#include <sstream>

using namespace dk;
namespace
{
Json request(std::string method, Json parameters = Json::object(), Json id = 1)
{
    return {{"jsonrpc", "2.0"},
            {"id", std::move(id)},
            {"method", std::move(method)},
            {"params", std::move(parameters)}};
}
struct Protocol
{
    SceneTestFiles files;
    std::unique_ptr<Runtime> runtime;
    Protocol()
    {
        auto r = Runtime::create(files.root);
        REQUIRE(r);
        runtime = std::move(*r);
    }
    Json call(const Json &value, bool automatic = false)
    {
        auto r = dispatch_json_rpc(*runtime, value, automatic);
        REQUIRE(r.response);
        return *r.response;
    }
};
} // namespace
TEST_CASE("RPC distinguishes unknown methods parameters and business errors")
{
    Protocol p;
    REQUIRE(p.call(request("missing"))["error"]["code"] == -32601);
    REQUIRE(p.call(request("commands.list", Json::array()))["error"]["code"] == -32602);
    REQUIRE(p.call(request("scene.query"))["error"]["code"] == -32002);
    auto created = p.call(request("scene.new"));
    REQUIRE(created.contains("result"));
    REQUIRE(p.call(request("entity.create"))["error"]["code"] == -32602);
    auto result = p.call(request("entity.create"), true);
    REQUIRE(result["result"]["value"]["created_id"].is_string());
    auto state = created["result"]["value"];
    auto guard = Json{{"document_id", state["document_id"]}, {"revision", state["revision"]}};
    auto stale = p.call(request("entity.create", {{"guard", guard}}), true);
    REQUIRE(stale["error"]["code"] == -32007);
    REQUIRE(stale["error"]["data"]["engine_name"] == "conflict");
}
TEST_CASE("RPC validates envelopes IDs and strict JSON parsing")
{
    Protocol p;
    for (const auto &id : {Json(true), Json(1.0), Json::array(), Json::object(),
                           Json(std::numeric_limits<std::uint64_t>::max())})
        REQUIRE(p.call(request("commands.list", Json::object(), id))["error"]["code"] == -32600);
    for (const auto &id :
         {Json(nullptr), Json("request-中文"), Json(std::numeric_limits<std::int64_t>::min()),
          Json(std::numeric_limits<std::int64_t>::max())})
        REQUIRE(p.call(request("commands.list", Json::object(), id))["id"] == id);
    auto invalid = request("commands.list");
    invalid["unknown"] = true;
    REQUIRE(p.call(invalid)["error"]["code"] == -32600);
    invalid["jsonrpc"] = "1.0";
    REQUIRE(p.call(invalid)["id"].is_null());
    REQUIRE(dispatch_json_line(*p.runtime, R"({"jsonrpc":"2.0","id":1,"id":2,"method":"commands.list"})")
                .response->at("error")["code"] == -32700);
    REQUIRE(dispatch_json_line(*p.runtime, "{} trailing").response->at("error")["code"] == -32700);
}
TEST_CASE("RPC notifications and mixed batches respond only when required")
{
    Protocol p;
    auto notification = request("commands.list");
    notification.erase("id");
    REQUIRE_FALSE(dispatch_json_rpc(*p.runtime, notification).response);
    notification["method"] = "missing";
    auto failed = dispatch_json_rpc(*p.runtime, notification);
    REQUIRE(failed.failed);
    REQUIRE_FALSE(failed.response);
    Json batch = Json::array({notification, request("commands.list", Json::object(), "a"), false});
    auto result = p.call(batch);
    REQUIRE(result.size() == 2);
    REQUIRE(result[0]["id"] == "a");
    REQUIRE(result[1]["error"]["code"] == -32600);
    REQUIRE(p.call(Json::array())["error"]["code"] == -32600);
    REQUIRE_FALSE(dispatch_json_rpc(*p.runtime, Json::array({notification, notification})).response);
    REQUIRE(p.call(Json(std::vector<Json>(129, notification)))["error"]["code"] == -32600);
}
TEST_CASE("JSON line transport drains oversized input and handles final lines and stream failures")
{
    Protocol p;
    std::istringstream input(" \r\n" + std::string(1024 * 1024 + 1, 'x') + "\n" +
                             request("commands.list").dump());
    std::ostringstream output, diagnostics;
    REQUIRE(run_json_lines(*p.runtime, input, output, diagnostics) == 1);
    REQUIRE(diagnostics.str().empty());
    std::istringstream responses(output.str());
    std::string line;
    REQUIRE(static_cast<bool>(std::getline(responses, line)));
    REQUIRE(Json::parse(line)["error"]["code"] == -32700);
    REQUIRE(static_cast<bool>(std::getline(responses, line)));
    REQUIRE(Json::parse(line).contains("result"));
    REQUIRE_FALSE(static_cast<bool>(std::getline(responses, line)));
    std::istringstream broken;
    broken.setstate(std::ios::badbit);
    REQUIRE(run_json_lines(*p.runtime, broken, output, diagnostics) == 3);
    std::istringstream valid(request("commands.list").dump());
    std::ostringstream rejected;
    rejected.setstate(std::ios::badbit);
    REQUIRE(run_json_lines(*p.runtime, valid, rejected, diagnostics) == 3);
}

TEST_CASE("synchronous task IDs expose terminal status and bounded retention")
{
    Protocol p;
    auto success = p.call(request("runtime.capabilities"));
    REQUIRE(success["result"]["status"] == "succeeded");
    REQUIRE(success["result"]["value"]["async_tasks"] == false);
    auto first_id = success["result"]["task_id"];
    REQUIRE(TaskId::parse(first_id.get<std::string>()));
    auto metadata = p.call(request("tasks.get", {{"id", first_id}}))["result"]["value"];
    REQUIRE(metadata["method"] == "runtime.capabilities");
    REQUIRE(metadata["document"].is_null());
    auto failure = p.call(request("scene.query"));
    REQUIRE(failure["error"]["data"]["status"] == "failed");
    auto failed_id = failure["error"]["data"]["task_id"];
    metadata = p.call(request("tasks.get", {{"id", failed_id}}))["result"]["value"];
    REQUIRE(metadata["error_code"] == static_cast<unsigned>(ErrorCode::invalid_state));
    REQUIRE(metadata["status"] == "failed");
    REQUIRE_FALSE(p.call(request("missing"))["error"].contains("data"));
    for (int i = 0; i < 260; ++i)
    {
        auto r = dispatch_json_rpc(*p.runtime, request("runtime.capabilities"));
        REQUIRE_FALSE(r.failed);
    }
    auto list = p.call(request("tasks.list"))["result"]["value"];
    REQUIRE(list.size() == 256);
    REQUIRE(p.call(request("tasks.get", {{"id", first_id}}))["error"]["code"] == -32003);
    p.call(request("scene.new"));
    auto changed = p.call(request("entity.create"), true);
    metadata = p.call(request("tasks.get", {{"id", changed["result"]["task_id"]}}))["result"]["value"];
    REQUIRE(metadata["document"]["revision"] == 1);
}

TEST_CASE("stdio EOF recovers errors and shutdown refuses later batch mutations")
{
    Protocol p;
    std::istringstream input("{\n" + request("runtime.capabilities").dump() + "\n");
    std::ostringstream output, diagnostics;
    REQUIRE(run_json_lines(*p.runtime, input, output, diagnostics, false, true) == 0);
    REQUIRE(diagnostics.str().empty());
    Json batch = Json::array({request("runtime.shutdown"), request("scene.new")});
    auto results = p.call(batch);
    REQUIRE(results[0]["result"]["value"]["stopping"] == true);
    REQUIRE(results[1]["error"]["code"] == -32002);
    REQUIRE_FALSE(results[1]["error"]["data"].contains("task_id"));
    REQUIRE(p.runtime->stopping());
    REQUIRE_FALSE(p.runtime->dispatch("commands.list", Json::object()));
    Protocol notification;
    auto stop = request("runtime.shutdown");
    stop.erase("id");
    std::istringstream queued(stop.dump() + "\n" + request("scene.new").dump());
    std::ostringstream no_response;
    REQUIRE(run_json_lines(*notification.runtime, queued, no_response, diagnostics, false, true) == 0);
    REQUIRE(no_response.str().empty());
    REQUIRE(queued.peek() == '{');
}

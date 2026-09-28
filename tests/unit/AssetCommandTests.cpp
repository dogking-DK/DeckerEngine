#include "GltfTestSupport.hpp"
#include <dk/runtime/Runtime.hpp>

using namespace dk;
namespace {
struct Commands {
    GltfFixture f;
    std::unique_ptr<Runtime> runtime;
    Commands() {
        f.save(); f.files.write("project.json", R"({"format":"DeckerProject","version":1,"name":"CPU","scene":"scene.json","assets":[]})");
        auto r = Runtime::create(f.files.root); REQUIRE(r); runtime = std::move(*r);
    }
    Result<Json> call(std::string_view method, Json p = Json::object(), bool auto_guard = false) {
        auto result = runtime->dispatch(method,p,auto_guard); if (!result) return std::unexpected(result.error()); return std::move(result->result);
    }
    Json value(std::string_view method, Json p = Json::object(), bool auto_guard = false) {
        auto result = call(method,std::move(p),auto_guard); INFO(method);
        INFO((result ? std::string{"success"} : result.error().message));
        REQUIRE(result); return std::move(*result);
    }
    Json finish(const Json& id) {
        const auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds(10);
        for (;;) {
            auto waited = value("jobs.wait",{{"id",id},{"timeout_ms",1000}});
            if (!waited["timed_out"].get<bool>()) return waited["job"];
            REQUIRE(std::chrono::steady_clock::now() < deadline);
        }
    }
};
}
TEST_CASE("asset command discovery schemas and effects describe the implemented async limits")
{
    Commands c; auto caps = c.value("runtime.capabilities"); CHECK(caps["async_jobs"] == true); CHECK(caps["async_tasks"] == false);
    CHECK(caps["job_limits"]["terminal"] == 256);
    auto commands = c.value("commands.list"); CHECK(commands.size() == 33);
    for (const auto name : {"assets.open","assets.catalog","assets.import","assets.register","assets.rename","assets.load","assets.status","assets.unload","jobs.get","jobs.wait","jobs.cancel"}) {
        auto desc = c.value("commands.describe",{{"name",name}}); CHECK(desc["undoable"] == false);
        CHECK(desc["parameters"]["additionalProperties"] == false);
        REQUIRE(schema::check(desc["parameters"])); REQUIRE(schema::check(desc["result"]));
    }
    auto unknown = JobId::generate()->to_string(); CHECK_FALSE(c.call("jobs.wait",{{"id",unknown},{"timeout_ms",1001}}));
    CHECK(c.call("jobs.get",{{"id",unknown}}).error().code == ErrorCode::not_found);
    CHECK(c.call("assets.catalog").error().code == ErrorCode::invalid_state);
    CHECK_FALSE(c.call("assets.import",{{"source","assets/模型.gltf"},{"unknown",true}}));
}
TEST_CASE("asset commands import without scene then register load unload and query separate JobId")
{
    Commands c;
    auto submitted = c.runtime->dispatch("assets.import",{{"source","assets/模型.gltf"}}); REQUIRE(submitted); REQUIRE(submitted->result);
    const auto job = submitted->result->at("job_id"); CHECK(job != submitted->task_id.to_string());
    CHECK(c.value("tasks.get",{{"id",submitted->task_id.to_string()}})["status"] == "succeeded");
    auto finished = c.finish(job); REQUIRE(finished["state"] == "succeeded"); const auto root = finished["result"]["root_id"];
    CHECK(c.call("scene.query").error().code == ErrorCode::invalid_state);
    CHECK(c.call("assets.load",{{"id",root}}).error().code == ErrorCode::invalid_state);
    auto opened = c.value("assets.open",{{"manifest","project.json"}});
    auto registered = c.value("assets.register",{{"source","assets/模型.gltf"}},true); CHECK(registered["total"] == 2);
    CHECK(c.call("assets.register",{{"source","assets/模型.gltf"},{"guard",opened["guard"]}}).error().code == ErrorCode::conflict);
    auto catalog = c.value("assets.catalog",{{"limit",1}}); CHECK(catalog["has_more"] == true); CHECK(catalog["records"].size() == 1);
    auto loading = c.value("assets.load",{{"id",root}}); REQUIRE(loading["state"] == "loading");
    CHECK(c.finish(loading["job_id"])["state"] == "succeeded");
    auto ready = c.value("assets.status",{{"id",root}}); CHECK(ready["state"] == "ready"); CHECK(ready["artifact"]["cache_hit"] == true);
    CHECK(c.value("jobs.cancel",{{"id",loading["job_id"]}})["accepted"] == false);
    CHECK(c.value("assets.unload",{{"id",root}})["state"] == "unloaded");
    CHECK(c.value("assets.status",{{"id",root}})["artifact"].is_null());
    auto failed = c.value("assets.import",{{"source","assets/missing.gltf"}}); CHECK(c.finish(failed["job_id"])["state"] == "failed");
    CHECK(c.value("runtime.capabilities")["async_jobs"] == true);
}
TEST_CASE("catalog persistence synchronizes Scene mappings without changing document state or history")
{
    Commands c; auto scene = c.value("scene.new",{{"name","CPU"}});
    c.value("scene.save",Json::object(),true); c.value("project.save",{{"manifest","project.json"}},true);
    c.value("assets.open",{{"manifest","project.json"}});
    const auto before = c.value("scene.query")["state"], history = c.value("history.status");
    auto imported = c.value("assets.import",{{"source","assets/模型.gltf"}}); auto finished = c.finish(imported["job_id"]); REQUIRE(finished["state"] == "succeeded");
    const auto root = finished["result"]["root_id"];
    c.value("assets.register",{{"source","assets/模型.gltf"}},true);
    CHECK(c.value("scene.query")["state"] == before); CHECK(c.value("history.status") == history);
    c.value("project.save",{{"manifest","project.json"}},true);
    auto entity = c.value("entity.create",Json::object(),true);
    c.value("entity.set_assets",{{"id",entity["created_id"]},{"assets",Json::array({{{"id",root},{"kind","mesh"}}})}},true);
    c.value("assets.rename",{{"source","assets/模型.gltf"},{"target","assets/renamed.gltf"}},true);
    c.value("scene.save",Json::object(),true); c.value("project.save",{{"manifest","project.json"}},true);
    auto project = Project::open(c.f.files.root,"project.json"); REQUIRE(project); CHECK(project->description().assets.size() == 2);
    for (const auto& record : project->description().assets) CHECK(record.path == "assets/renamed.gltf");
    auto guard = c.value("assets.catalog")["guard"];
    CHECK_FALSE(c.call("scene.transaction",{{"commands",Json::array({{{"method","assets.unload"},{"params",{{"id",root}}}}})}},true));
    CHECK_FALSE(c.call("assets.open",{{"manifest","project.json"}}));
    auto reopened = c.value("assets.open",{{"manifest","project.json"},{"guard",guard}}); CHECK(reopened["guard"]["session_id"] != guard["session_id"]);
}

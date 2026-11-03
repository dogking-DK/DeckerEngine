#include "SceneTestFiles.hpp"
#include <dk/automation/JsonLines.hpp>
#include <algorithm>

using namespace dk;
namespace {
Json call(Runtime& runtime, std::string_view method, const Json& params=Json::object()) {
    auto execution=runtime.dispatch(method,params); REQUIRE(execution); REQUIRE(execution->result);
    return std::move(*execution->result);
}
Json guard(const Json& state) { return {{"document_id",state["document_id"]},{"revision",state["revision"]}}; }
}
TEST_CASE("catalog reads preserve tasks scene history files and simulation") {
    SceneTestFiles files;
    auto runtime=Runtime::create(files.root).value();
    const auto empty_version=runtime->command_catalog_version();
    auto initial=call(*runtime,"scene.new");
    REQUIRE(call(*runtime,"scene.save",{{"guard",guard(initial)}}).is_object());
    const auto before=call(*runtime,"scene.query"), history=call(*runtime,"history.status");
    const auto disk=read_file_bytes(files.root/"scene.json").value();
    const auto tasks=call(*runtime,"tasks.list");
    for (int i=0;i<100;++i) {
        const auto snapshot=runtime->command_catalog();
        REQUIRE(snapshot.version==empty_version); REQUIRE_FALSE(snapshot.commands.empty());
        CHECK(std::ranges::is_sorted(snapshot.commands,{},&CommandDescriptor::name));
    }
    const auto after_tasks=call(*runtime,"tasks.list");
    CHECK(after_tasks.size()==tasks.size()+1); CHECK(after_tasks.back()["method"]=="tasks.list");
    CHECK(call(*runtime,"scene.query")==before); CHECK(call(*runtime,"history.status")==history);
    CHECK(read_file_bytes(files.root/"scene.json").value()==disk);
    CHECK_FALSE(runtime->simulation_state().run.has_value());
    const auto query=call(*runtime,"commands.describe",{{"name","scene.query"}});
    const auto& fields=query["help"]["fields"];
    CHECK(call(*runtime,"scene.query",{{"offset",fields["/properties/offset"]["default"]},{"limit",fields["/properties/limit"]["default"]}})==before);
    const auto environment=call(*runtime,"commands.describe",{{"name","environment.set"}});
    CHECK_FALSE(environment["help"]["fields"]["/properties/environment/properties/sun/properties/irradiance"].contains("default"));
}

TEST_CASE("catalog RPC discovery keeps envelopes legacy descriptors and optional help compatible") {
    SceneTestFiles files;
    auto runtime=Runtime::create(files.root).value();
    auto snapshot=runtime->command_catalog();
    const auto list=call(*runtime,"commands.list");
    REQUIRE(list.size()==snapshot.commands.size());
    for (const auto& d : snapshot.commands) {
        const auto reply=dispatch_json_rpc(*runtime,{{"jsonrpc","2.0"},{"id",17},{"method","commands.describe"},{"params",{{"name",d.name}}}});
        REQUIRE(reply.response); const auto& envelope=*reply.response;
        REQUIRE(envelope.contains("result")); CHECK(envelope["id"]==17); CHECK(envelope["result"]["status"]=="succeeded");
        CHECK(TaskId::parse(envelope["result"]["task_id"].get<std::string>()));
        auto value=envelope["result"]["value"];
        if (d.help) { REQUIRE(value.contains("help")); CHECK(value["help"]==*d.help); value.erase("help"); }
        else CHECK_FALSE(value.contains("help"));
        CHECK(value==Json{{"name",d.name},{"description",d.description},{"parameters",d.parameters},{"result",d.result},
            {"effect",effect_name(d.effect)},{"undoable",d.undoable}});
        const auto brief=std::ranges::find_if(list,[&](const Json& item) { return item["name"]==d.name; });
        REQUIRE(brief!=list.end()); CHECK(brief->size()==4);
    }
    CHECK(runtime->command_catalog_version()==snapshot.version);
    const auto unknown=dispatch_json_rpc(*runtime,{{"jsonrpc","2.0"},{"id",1},{"method","commands.describe"},{"params",{{"name","not.registered"}}}});
    REQUIRE(unknown.response); CHECK((*unknown.response)["error"]["code"]==-32003);
    call(*runtime,"runtime.shutdown"); CHECK(runtime->command_catalog_version()==snapshot.version);
    snapshot=runtime->command_catalog(); runtime.reset();
    REQUIRE_FALSE(snapshot.commands.empty());
    auto replacement=Runtime::create(files.root).value();
    CHECK(replacement->command_catalog_version().registry_id!=snapshot.version.registry_id);
}

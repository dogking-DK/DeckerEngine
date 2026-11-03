#include <dk/commands/CommandRegistry.hpp>
#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <limits>

using namespace dk;
namespace {
CommandDescriptor descriptor(Json help) {
    auto count = schema::integer(); count["minimum"]=1; count["maximum"]=20;
    return {"test.help","Test help",schema::object({{"count",count},{"text",schema::nullable(schema::string())}}, {"count"}),
        schema::object(),CommandEffect::query,false,std::move(help)};
}
Json help(Json entry, std::string path="/properties/count") {
    return {{"version",1},{"fields",{{std::move(path),std::move(entry)}}}};
}
auto handler = [](const Json&) -> Result<Json> { return Json::object(); };
}

TEST_CASE("help discovery preserves legacy fields and never injects defaults") {
    CommandRegistry registry;
    auto d = descriptor(help({{"label","数量"},{"description","Number of items.\nRead only."},{"unit","items"},
        {"default",3},{"examples",Json::array({1,20})},{"aliases",Json::array({"count","条数"})}}));
    (*d.help)["fields"]["/properties/text"]={{"default",nullptr}};
    int calls=0;
    REQUIRE(registry.add(d,[&](const Json& p)->Result<Json> { ++calls; CHECK_FALSE(p.contains("text")); return Json::object(); }));
    const auto description=registry.describe(d.name); REQUIRE(description);
    CHECK((*description)["help"]==*d.help); CHECK((*description)["parameters"]==d.parameters);
    CHECK((*description)["result"]==d.result); CHECK((*description)["effect"]=="query"); CHECK_FALSE((*description)["undoable"].get<bool>());
    CHECK(registry.execute("commands.describe",{{"name",d.name}}).value()==*description);
    CHECK_FALSE(registry.execute(d.name)); CHECK(calls==0); // help default cannot fill a required field
    REQUIRE(registry.execute(d.name,{{"count",2}})); CHECK(calls==1);
    (*d.help)["fields"]["/properties/count"]["label"]="changed";
    CHECK(registry.describe(d.name).value()==*description);
    for (const auto& item : registry.list()) {
        CHECK(item.size()==4); CHECK_FALSE(item.contains("help")); CHECK_FALSE(item.contains("parameters"));
    }
    REQUIRE(registry.add({"test.legacy","No help",schema::object(),schema::object()},handler));
    CHECK(registry.describe("test.legacy")->size()==6);
    CHECK_FALSE(registry.describe("test.legacy")->contains("help"));
    CHECK_FALSE(schema::check({{"type","string"},{"description","still not a schema keyword"}}));
}

TEST_CASE("help invalid metadata registration leaves catalog and execution unchanged") {
    CommandRegistry registry;
    const auto before=registry.catalog_version();
    const auto list=registry.list();
    std::vector<Json> bad={nullptr,Json::array(),Json::object(),{{"version",1}},{{"version",1},{"fields",Json::array()}},
        {{"version",1.0},{"fields",Json::object()}},{{"version",2},{"fields",Json::object()}},
        {{"version",1},{"fields",Json::object()},{"extra",1}},help(Json::object()),help(1),help({{"typo","value"}}),
        help({{"default",0}}),help({{"default",1.0}}),help({{"default","1"}}),help({{"examples",{1,21}}}),
        help({{"examples",Json::object()}}),help({{"examples",std::vector<int>(9,1)}}),
        help({{"aliases",{"same","same"}}}),help({{"aliases",{1}}}),help({{"aliases",std::vector<std::string>(17,"a")}}),
        help({{"label",std::string(257,'x')}}),help({{"unit",std::string(65,'x')}}),help({{"description",std::string(4097,'x')}}),
        help({{"label",""}}),help({{"label",std::string("a\0b",3)}}),help({{"label","a\nb"}}),
        help({{"label",std::string("\xc0\xaf")}}),help({{"completion_provider","scene.entity"}})};
    for (const auto* pointer : {"/properties","/properties/count/type","/properties/missing","/items","properties/count","/properties/~2bad"})
        bad.push_back(help({{"label","bad path"}},pointer));
    Json nested=0; for(int i=0;i<65;++i) nested=Json::array({std::move(nested)});
    bad.push_back(help({{"examples",Json::array({nested})}}));
    for (const auto& value : bad) {
        const auto result=registry.add(descriptor(value),handler);
        REQUIRE_FALSE(result); CHECK(result.error().code==ErrorCode::invalid_argument);
        CHECK(registry.catalog_version()==before); CHECK(registry.list()==list);
        CHECK_FALSE(registry.describe("test.help"));
    }
    REQUIRE(registry.add(descriptor(help({{"label","valid retry"}})),handler));
    CHECK(registry.catalog_version().revision==before.revision+1);
}

TEST_CASE("help pointers follow schema nodes including escaped names and array items") {
    CommandRegistry registry;
    const auto parameters=schema::object({{"a/b~c",schema::array(schema::object({{"中文",schema::integer()}}))}});
    const auto metadata=Json{{"version",1},{"fields",{
        {"",{{"label","参数根"}}},
        {"/properties/a~1b~0c/items/properties/中文",{{"default",std::numeric_limits<std::uint64_t>::max()},{"examples",Json::array({-1})}}}
    }}};
    REQUIRE(registry.add({"test.pointer","Pointers",parameters,schema::object(),CommandEffect::query,false,std::optional<Json>{std::in_place,metadata}},handler));
    const auto exposed=registry.describe("test.pointer").value();
    CHECK(exposed["help"]==metadata);
    CHECK(exposed["help"]["fields"]["/properties/a~1b~0c/items/properties/中文"]["default"].dump()=="18446744073709551615");
    auto bad=metadata; bad["fields"]["/properties/a~1b~0c/items/properties/中文"]["default"]=1.0;
    CHECK_FALSE(registry.add({"test.bad","Integer default",parameters,schema::object(),CommandEffect::query,false,bad},handler));
}

TEST_CASE("help encoded size boundary rejects whole registration before publication") {
    Json fields=Json::object(), properties=Json::object();
    for (int i=0;i<16;++i) {
        const auto key="field"+std::to_string(i); properties[key]=schema::integer();
        fields["/properties/"+key]={{"description",std::string(4096,'x')}};
    }
    Json metadata={{"version",1},{"fields",fields}};
    auto& tail=metadata["fields"]["/properties/field15"]["description"].get_ref<std::string&>();
    REQUIRE(metadata.dump().size()>64*1024);
    tail.resize(tail.size()-(metadata.dump().size()-64*1024));
    REQUIRE(metadata.dump().size()==64*1024);
    CommandRegistry registry;
    REQUIRE(registry.add({"test.limit","Limit",schema::object(properties),schema::object(),CommandEffect::query,false,metadata},handler));
    tail+='x'; REQUIRE(metadata.dump().size()==64*1024+1);
    const auto version=registry.catalog_version();
    CHECK_FALSE(registry.add({"test.over","Limit",schema::object(properties),schema::object(),CommandEffect::query,false,metadata},handler));
    CHECK(registry.catalog_version()==version); CHECK_FALSE(registry.describe("test.over"));
}

TEST_CASE("catalog snapshots own descriptors and distinguish registries and successful changes") {
    CommandCatalogSnapshot retained;
    {
        CommandRegistry registry,other;
        const auto initial=registry.catalog_version();
        CHECK_FALSE(initial.registry_id.is_nil()); CHECK(initial.registry_id!=other.catalog_version().registry_id);
        REQUIRE(registry.register_completion_provider("scene.entity"));
        REQUIRE(registry.register_completion_provider("assets.entry"));
        const auto providers_version=registry.catalog_version(); CHECK(providers_version.revision==initial.revision+2);
        for (const auto* name : {"scene.entity","","1bad","bad-name","中文"}) CHECK_FALSE(registry.register_completion_provider(name));
        CHECK(registry.catalog_version()==providers_version);
        auto d=descriptor(help({{"label","Count"},{"completion_provider","scene.entity"}}));
        int calls=0;
        REQUIRE(registry.add(d,[&](const Json&)->Result<Json> { ++calls; return Json::object(); }));
        retained=registry.catalog_snapshot();
        CHECK(retained.version==registry.catalog_version()); CHECK(retained.version.revision==providers_version.revision+1);
        CHECK(retained.completion_providers==std::vector<std::string>{"assets.entry","scene.entity"});
        REQUIRE(std::ranges::is_sorted(retained.commands,{},&CommandDescriptor::name));
        auto modified=retained; modified.commands.back().help->at("fields").clear(); modified.commands.clear(); modified.completion_providers.clear();
        REQUIRE(registry.catalog_snapshot().commands.back().help==d.help);
        CHECK_FALSE(registry.add(d,handler)); CHECK(registry.catalog_version()==retained.version);
        REQUIRE(registry.add({"test.next","Next",schema::object(),schema::object()},handler));
        CHECK(registry.catalog_version().revision==retained.version.revision+1);
        CHECK(retained.commands.size()+1==registry.catalog_snapshot().commands.size()); CHECK(calls==0);
    }
    REQUIRE(retained.commands.back().name=="test.help");
    CHECK(retained.commands.back().help->at("fields")["/properties/count"]["label"]=="Count");
}

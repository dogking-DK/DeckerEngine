#include "ConsoleAssist.hpp"
#include "SceneTestFiles.hpp"
#include <dk/editor/Workspace.hpp>
#include <dk/editor/Console.hpp>
#include <algorithm>
#include <limits>
#include <tuple>

using namespace dk;
using namespace dk::editor;
using namespace dk::editor::detail;
namespace {
ConsoleEditState edit(std::string text) { const auto n=text.size(); return {std::move(text),n,n,n,1}; }
CommandCatalogSnapshot catalog() {
    CommandRegistry registry;
    const auto handler=[](const Json&)->Result<Json> { CHECK(false); return Json::object(); };
    auto values=schema::string(); values["enum"]=Json::array({"pathtrace","raster","中文 空格","a\"b\\c","true","123"});
    auto numbers=schema::integer(); numbers["enum"]=Json::array({std::numeric_limits<std::uint64_t>::max()});
    auto limit=schema::integer(); limit["minimum"]=1; limit["maximum"]=256;
    auto parameters=schema::object({{"renderer",values},{"enabled",schema::boolean()},{"nullable",schema::nullable(schema::string())},
        {"seed",numbers},{"guard",schema::object()},{"name",schema::string()},{"limit",limit},{"offset",schema::integer()}},{"renderer"});
    Json help={{"version",1},{"fields",{{"/properties/limit",{{"label","分页数量"},{"description","Maximum entities"},{"unit","entities"},{"default",128},{"examples",Json::array({1,20})}}}}}};
    REQUIRE(registry.add({"render.capture","Capture help",parameters,schema::object(),CommandEffect::query,false,std::move(help)},handler));
    REQUIRE(registry.add({"scene.query","Query",schema::object({{"limit",limit},{"offset",schema::integer()}}),schema::object()},handler));
    return registry.catalog_snapshot();
}
ConsoleEditState choose(const CommandCatalogSnapshot& cat, ConsoleEditState state, std::string_view insertion) {
    const auto assistance=assist_console(cat,state);
    const auto candidate=std::ranges::find(assistance.candidates,insertion,&ConsoleCandidate::insert);
    REQUIRE(candidate!=assistance.candidates.end());
    auto next=accept_console_candidate(assistance,std::size_t(candidate-assistance.candidates.begin()),state,cat.version);
    REQUIRE(next); return *next;
}
}
TEST_CASE("console assistance locates command and top-level keys without altering other tokens") {
    const auto cat=catalog();
    CHECK(choose(cat,edit("scene.qu"),"scene.query").text=="scene.query");
    CHECK(choose(cat,edit("scene.query li"),"limit=").text=="scene.query limit=");
    auto middle=edit("render.capture name='中文 空格' li =20 guard={\"revision\":18446744073709551615}");
    middle.cursor=middle.selection_start=middle.selection_end=middle.text.find("li =")+2;
    CHECK(choose(cat,middle,"limit").text=="render.capture name='中文 空格' limit =20 guard={\"revision\":18446744073709551615}");
    const auto list=assist_console(cat,edit("render.capture name='kept' guard={\"x\":[1, 2]} "));
    CHECK(std::ranges::none_of(list.candidates,[](const auto& c) { return c.insert=="name=" || c.insert=="guard="; }));
    auto after=edit("scene.query li offset=2"); after.cursor=after.selection_start=after.selection_end=14;
    CHECK(choose(cat,after,"limit=").text=="scene.query limit= offset=2");
}
TEST_CASE("console assistance preserves enum strings booleans null and full integer precision") {
    const auto cat=catalog();
    for (const auto& [source,insertion,value] : std::vector<std::tuple<std::string,std::string,Json>>{
        {"renderer=pa","\"pathtrace\"","pathtrace"},{"renderer='中文","\"中文 空格\"","中文 空格"},
        {"renderer=\"a\\\"","\"a\\\"b\\\\c\"","a\"b\\c"},{"renderer=tru","\"true\"","true"},
        {"renderer=12","\"123\"","123"},{"enabled=f","false",false},{"nullable=n","null",nullptr},
        {"seed=184","18446744073709551615",std::numeric_limits<std::uint64_t>::max()}}) {
        const auto completed=choose(cat,edit("render.capture "+source),insertion);
        auto parsed=parse_console_command(completed.text); REQUIRE(parsed);
        CHECK(parsed->params.begin().value()==value);
    }
    CHECK(assist_console(cat,edit("render.capture limit=")).candidates.empty());
    auto hints=assist_console(cat,edit("render.capture limit="));
    for (const auto* s : {"optional","integer","minimum: 1","maximum: 256","unit: entities","default: 128","examples:"}) CHECK(hints.hint.find(s)!=std::string::npos);
    CHECK(assist_console(cat,edit("render.capture renderer=")).hint.find("required")!=std::string::npos);
    CHECK(assist_console(cat,edit("scene.query limit=")).hint.find("default:")==std::string::npos);
}
TEST_CASE("console assistance rejects invalid positions nested inputs and stale selections") {
    const auto cat=catalog();
    for (const auto* text : {"render.capture guard={\"lim", "render.capture guard=[1,", "render.capture name=a;bad", "render.capture name=\"a\"suffix", "unknown li"})
        CHECK(assist_console(cat,edit(text)).candidates.empty());
    auto selected=edit("scene.query"); selected.selection_start=selected.cursor=0;
    CHECK(choose(cat,selected,"render.capture").text=="render.capture");
    selected.selection_start=1;
    CHECK(assist_console(cat,selected).candidates.empty());
    auto unicode=edit("render.capture renderer='中文");
    unicode.cursor=unicode.selection_start=unicode.selection_end=unicode.text.size()-1;
    CHECK(assist_console(cat,unicode).candidates.empty());
    auto quoted=edit("render.capture renderer='中文 空格' limit=20");
    quoted.selection_start=quoted.cursor=quoted.text.find('\'');
    quoted.selection_end=quoted.text.find('\'',quoted.selection_start+1)+1;
    CHECK(choose(cat,quoted,"\"pathtrace\"").text=="render.capture renderer=\"pathtrace\" limit=20");
    std::swap(quoted.selection_start,quoted.selection_end); quoted.cursor=quoted.selection_start;
    CHECK(choose(cat,quoted,"\"raster\"").text=="render.capture renderer=\"raster\" limit=20");
    quoted.selection_end=0;
    CHECK(assist_console(cat,quoted).candidates.empty());
    CHECK(assist_console(cat,edit(std::string("scene.\xc0\xaf"))).candidates.empty());
    auto in_value=edit("render.capture renderer=pa limit=20");
    in_value.cursor=in_value.selection_start=in_value.selection_end=in_value.text.find("pa limit")+2;
    CHECK(choose(cat,in_value,"\"pathtrace\"").text=="render.capture renderer=\"pathtrace\" limit=20");
    const auto original=edit("scene.qu"); const auto list=assist_console(cat,original);
    for(int change=0;change<5;++change) {
        auto current=original; auto version=cat.version;
        if(change==0) current.text+='x'; if(change==1) --current.cursor;
        if(change==2) --current.selection_start; if(change==3) ++current.revision; if(change==4) ++version.revision;
        CHECK_FALSE(accept_console_candidate(list,0,current,version)); CHECK(current.text==(change==0 ? "scene.qux" : "scene.qu"));
    }
    auto replacement=cat.version; replacement.registry_id=CommandCatalogId::generate().value();
    CHECK_FALSE(accept_console_candidate(list,0,original,replacement));
}
TEST_CASE("console assistance bounds candidates descriptions and replacement bytes") {
    auto cat=catalog();
    auto state=edit("scene.qu"); state.text+=std::string(console_input_limit-3-state.text.size(),' ');
    const auto accepted=choose(cat,state,"scene.query"); CHECK(accepted.text.size()==console_input_limit);
    state.text+=' '; auto list=assist_console(cat,state);
    REQUIRE_FALSE(list.candidates.empty()); CHECK_FALSE(accept_console_candidate(list,0,state,cat.version));
    CHECK(state.text.size()==console_input_limit-2);
    state=edit(std::string(console_input_limit+1,'x')); CHECK(assist_console(cat,state).candidates.empty());
    for(int i=99;i>=0;--i) cat.commands.push_back({"many.command"+std::to_string(i),std::string(5000,'d'),schema::object(),schema::object()});
    const auto many=assist_console(cat,edit("many.")); REQUIRE(many.candidates.size()==32); CHECK(many.truncated);
    CHECK(std::ranges::is_sorted(many.candidates,{},&ConsoleCandidate::label));
    for(const auto& c : many.candidates) CHECK(c.description.size()<=2048);
}
TEST_CASE("console assistance reads actual workspace metadata without tasks drafts or scene changes") {
    SceneTestFiles files; std::filesystem::copy(DK_EDITOR_FIXTURE_DIR,files.root,std::filesystem::copy_options::recursive);
    auto model=Workspace::create(files.root).value(); REQUIRE(model->open("project.json"));
    REQUIRE(model->select(model->snapshot()->scene.entities().front().id));
    model->draft()->entity.name="keep draft"; model->draft()->modified=true;
    const auto first=model->execute_console("tasks.list").value(); const auto control=model->execute_console("tasks.list").value();
    const auto* snapshot=model->snapshot(); const auto history=model->history();
    const auto cat=model->command_catalog();
    for(int i=0;i<100;++i) CHECK(choose(cat,edit("environment.preset name=cl"),"\"clear\"").text=="environment.preset name=\"clear\"");
    CHECK(model->snapshot()==snapshot); CHECK(model->draft()->entity.name=="keep draft"); CHECK(model->pending());
    CHECK(model->history().undo_count==history.undo_count);
    const auto after=model->execute_console("tasks.list").value(); CHECK(after.size()-control.size()==control.size()-first.size());
}

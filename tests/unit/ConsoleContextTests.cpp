#include "ConsoleAssistCache.hpp"
#include "SceneTestFiles.hpp"
#include <dk/editor/Workspace.hpp>
#include <algorithm>

using namespace dk;
using namespace dk::editor;
using namespace dk::editor::detail;
namespace {
ConsoleEditState input(std::string text) { auto n=text.size(); return {std::move(text),n,n,n,1}; }
struct Fixture {
    SceneTestFiles files;
    std::unique_ptr<Workspace> model;
    CommandCatalogSnapshot catalog;
    Fixture() {
        std::filesystem::copy(DK_EDITOR_FIXTURE_DIR,files.root,std::filesystem::copy_options::recursive);
        model=Workspace::create(files.root).value(); REQUIRE(model->open("project.json")); catalog=model->command_catalog();
    }
};
std::string complete(const CommandCatalogSnapshot& catalog,ConsoleEditState edit,std::string_view insertion) {
    auto result=assist_console(catalog,edit);
    auto found=std::ranges::find(result.candidates,insertion,&ConsoleCandidate::insert);
    INFO(edit.text); INFO(insertion); REQUIRE(found!=result.candidates.end());
    return accept_console_candidate(result,std::size_t(found-result.candidates.begin()),edit,catalog.version).value().text;
}
bool has(const ConsoleAssistance& a,ConsoleDiagnosticKind kind,std::string_view message) {
    return std::ranges::any_of(a.diagnostics,[&](const auto& d){return d.kind==kind && d.message.find(message)!=std::string::npos;});
}
}
TEST_CASE("console context nested objects arrays and envelope use exact schema paths") {
    Fixture f;
    CHECK(complete(f.catalog,input("entity.set_transform transform={\"tr"),"\"translation\":")=="entity.set_transform transform={\"translation\":");
    CHECK(complete(f.catalog,input("entity.set_assets assets=[{\"kind\":\"me"),"\"mesh\"")=="entity.set_assets assets=[{\"kind\":\"mesh\"");
    CHECK(complete(f.catalog,input("{\"method\":\"scene.qu"),"\"scene.query\"")=="{\"method\":\"scene.query\"");
    CHECK(complete(f.catalog,input("{\"method\":\"scene.query\",\"params\":{\"li"),"\"limit\":")=="{\"method\":\"scene.query\",\"params\":{\"limit\":");
    auto before=input("{\"params\":{\"li\":20},\"method\":\"scene.query\"}");
    before.cursor=before.selection_start=before.selection_end=before.text.find("li\"")+2;
    CHECK(complete(f.catalog,before,"\"limit\"")=="{\"params\":{\"limit\":20},\"method\":\"scene.query\"}");
    auto existing=input("entity.set_transform transform={\"translation\":[1,2,3],\"sc");
    auto result=assist_console(f.catalog,existing);
    REQUIRE(result.candidates.size()==1); CHECK(result.candidates[0].insert=="\"scale\":");
}
TEST_CASE("console context diagnostics distinguish unfinished and definite errors with unicode positions") {
    Fixture f;
    for(const auto* s : {"scene.query limit=","entity.set_transform transform={","{\"method\":\"scene.query\",\"params\":{"}) {
        auto a=assist_console(f.catalog,input(s)); CHECK(has(a,ConsoleDiagnosticKind::incomplete,""));
        CHECK_FALSE(has(a,ConsoleDiagnosticKind::error,"Missing"));
    }
    CHECK(has(assist_console(f.catalog,input("scene.query limit=1.0")),ConsoleDiagnosticKind::error,"type"));
    CHECK(has(assist_console(f.catalog,input("scene.query limit=999")),ConsoleDiagnosticKind::error,""));
    CHECK(has(assist_console(f.catalog,input("scene.query limit=1 limit=2")),ConsoleDiagnosticKind::error,"Duplicate"));
    CHECK(has(assist_console(f.catalog,input("scene.query unknown=1")),ConsoleDiagnosticKind::error,"Unknown"));
    CHECK(has(assist_console(f.catalog,input("scene.query limit=18446744073709551616")),ConsoleDiagnosticKind::error,"precision"));
    CHECK(has(assist_console(f.catalog,input("entity.set_transform transform={\"scale\":[1,],\"rotation\":[]}")),ConsoleDiagnosticKind::error,"Trailing"));
    auto text=input("entity.set_name name='中文' mystery=1"); auto a=assist_console(f.catalog,text);
    const auto d=std::ranges::find_if(a.diagnostics,[](const auto& d){return d.message.find("Unknown")!=std::string::npos;});
    REQUIRE(d!=a.diagnostics.end()); CHECK(d->byte==text.text.find("mystery")); CHECK(d->column==d->byte-3);
    CHECK(a.diagnostics.size()<=16);
    CHECK(has(assist_console(f.catalog,input("entity.set_transform transform={\"scale\":[,]}")),ConsoleDiagnosticKind::error,"value"));
    CHECK(has(assist_console(f.catalog,input("entity.set_name name=\"a\" "+std::string(600,'x')+"=1")),ConsoleDiagnosticKind::error,"Unknown"));
    auto deep=assist_console(f.catalog,input("entity.set_assets assets="+std::string(66,'[')));
    CHECK(has(deep,ConsoleDiagnosticKind::error,"depth"));
    auto escaped=assist_console(f.catalog,input(R"(entity.set_transform transform={"scale":[1,2,3],"sc\u0061le":[1,2,3]})"));
    CHECK(has(escaped,ConsoleDiagnosticKind::error,"Duplicate"));
    auto bounded=assist_console(f.catalog,input("scene.query "+std::string(700,'x')+"=1"));
    for(const auto& diagnostic : bounded.diagnostics) CHECK(diagnostic.message.size()<=512);
    const auto overflow=input(R"(entity.set_transform transform={"translation":[1e999,0,0]})");
    auto huge=assist_console(f.catalog,overflow);
    REQUIRE_FALSE(huge.diagnostics.empty()); CHECK(huge.diagnostics.front().byte==overflow.text.find("1e999"));
}
TEST_CASE("console context reference bindings do not guess ids and filter asset kinds") {
    Fixture f; const auto version=f.model->console_reference_version();
    auto entity=assist_console(f.catalog,input("entity.get id="),version); CHECK(entity.provider=="scene.entity");
    CHECK(assist_console(f.catalog,input("entity.create id="),version).provider.empty());
    CHECK(assist_console(f.catalog,input("entity.set_name guard={\"document_id\":"),version).provider.empty());
    auto asset=assist_console(f.catalog,input("entity.set_assets assets=[{\"kind\":\"mesh\",\"id\":"),version);
    CHECK(asset.provider=="assets.reference"); CHECK(asset.kind_filter=="mesh");
    ConsoleReferencePage page{version,{{"00000000-0000-4000-8000-000000000001","same","mesh"},{"00000000-0000-4000-8000-000000000002","same","texture"}},0,2,false};
    add_console_reference_page(asset,page); REQUIRE(asset.candidates.size()==1);
    CHECK(asset.candidates[0].insert=="\"00000000-0000-4000-8000-000000000001\""); CHECK_FALSE(asset.loading);
    page.records[1].kind="mesh"; auto duplicates=assist_console(f.catalog,input("entity.set_assets assets=[{\"id\":"),version);
    add_console_reference_page(duplicates,page); REQUIRE(duplicates.candidates.size()==2);
    CHECK(duplicates.candidates[0].label!=duplicates.candidates[1].label);
    auto escaped=assist_console(f.catalog,input("entity.get id="),version);
    for(auto& record : page.records) record.name=std::string(150,'\n');
    add_console_reference_page(escaped,page); REQUIRE(escaped.candidates.size()==2);
    for(const auto& candidate : escaped.candidates) {
        CHECK(candidate.label.size()<=256); CHECK(candidate.label.find('\n')==std::string::npos);
        CHECK(candidate.label.find(Json::parse(candidate.insert).get<std::string>())!=std::string::npos);
    }
    auto old=version; ++old.revision;
    CHECK_FALSE(accept_console_candidate(asset,0,asset.source,f.catalog.version,old));
    auto later=input(R"(entity.set_assets assets=[{"id":"","kind":"texture"}])");
    later.cursor=later.selection_start=later.selection_end=later.text.find("\"\"")+1;
    CHECK(assist_console(f.catalog,later,version).kind_filter=="texture");
}
TEST_CASE("console context cache debounces paginates and rejects late publications") {
    Fixture f; ConsoleAssistCache cache; auto state=input("entity.get id="); auto version=f.model->console_reference_version();
    int reads=0;
    auto reader=[&](std::string_view provider,const ConsoleReferenceVersion& v,std::size_t offset,std::size_t limit)->Result<ConsoleReferencePage> {
        ++reads; CHECK(provider=="scene.entity"); CHECK(limit==128);
        ConsoleReferencePage p{v,{},offset,260,offset+limit<260};
        for(auto i=offset;i<std::min(std::size_t(260),offset+limit);++i) p.records.push_back({"id"+std::to_string(i),"entity"+std::to_string(i),"entity"});
        return p;
    };
    REQUIRE(cache.observe(state,f.catalog.version,version,0)); cache.poll(f.catalog,reader,99); CHECK_FALSE(cache.result()); CHECK(reads==0);
    cache.poll(f.catalog,reader,100); REQUIRE(cache.result()); CHECK(cache.result()->loading); CHECK(reads==1);
    auto old=*cache.result(); auto request=cache.request_id();
    cache.poll(f.catalog,reader,100); CHECK(reads==1);
    cache.poll(f.catalog,reader,101); cache.poll(f.catalog,reader,102); CHECK(reads==3);
    CHECK_FALSE(cache.result()->loading); CHECK(cache.result()->truncated); CHECK(cache.result()->candidates.size()==32);
    ++version.revision; REQUIRE(cache.observe(state,f.catalog.version,version,103)); CHECK_FALSE(cache.publish(old,request)); CHECK_FALSE(cache.result());
    auto new_state=state; new_state.text+='a'; ++new_state.cursor; new_state.selection_start=new_state.selection_end=new_state.cursor; ++new_state.revision;
    REQUIRE(cache.observe(new_state,f.catalog.version,version,110)); CHECK_FALSE(cache.publish(old,cache.request_id()));
}
TEST_CASE("console context readonly pages preserve tasks drafts files and reject document changes") {
    Fixture f;
    if(std::ranges::any_of(f.catalog.commands,[](const auto& d){return d.name=="assets.open";}))
        REQUIRE(f.model->execute_console("assets.open manifest=project.json"));
    REQUIRE(f.model->select(f.model->snapshot()->scene.entities()[0].id));
    f.model->draft()->modified=true; f.model->draft()->entity.name="保留草稿";
    auto version=f.model->console_reference_version(); const auto* scene=f.model->snapshot();
    auto disk=read_file_bytes(f.files.root/"scene.json").value();
    auto a=f.model->execute_console("tasks.list").value(); auto b=f.model->execute_console("tasks.list").value();
    version=f.model->console_reference_version(); // Explicit queries publish snapshots; reads below do not.
    for(int i=0;i<100;++i) {
        auto page=f.model->read_console_references("scene.entity",version,0,1); REQUIRE(page); REQUIRE(page->records.size()==1); CHECK(page->has_more);
        auto assets=f.model->read_console_references("assets.reference",version,0,1);
        REQUIRE(assets); CHECK(assets->records.size()==1);
        auto active=f.model->read_console_references("assets.any",version,0,1);
        if(version.assets.available) { REQUIRE(active); CHECK(active->records.size()==1); } else CHECK_FALSE(active);
    }
    CHECK(f.model->snapshot()==scene); CHECK(f.model->draft()->entity.name=="保留草稿"); CHECK(f.model->pending());
    CHECK(read_file_bytes(f.files.root/"scene.json").value()==disk);
    auto c=f.model->execute_console("tasks.list").value(); CHECK(c.size()-b.size()==b.size()-a.size());
    CHECK_FALSE(f.model->read_console_references("scene.entity",version,0,257));
    REQUIRE(f.model->open("project.json",true)); CHECK_FALSE(f.model->read_console_references("scene.entity",version,0));
}
TEST_CASE("console context deleted entities and replaced asset sessions invalidate identities") {
    Fixture f;
    auto version=f.model->console_reference_version();
    auto ids=f.model->read_console_references("scene.entity",version,0).value();
    const auto deleted=ids.records.front().id; // Leaf; deleting a parent is intentionally rejected by Scene.
    auto command=Json{{"method","entity.delete"},{"params",{{"id",deleted},{"guard",{{"document_id",version.document_id.to_string()},{"revision",version.revision}}}}}};
    auto removed=f.model->execute_console(command.dump()); INFO((removed ? "deleted" : removed.error().message)); REQUIRE(removed);
    CHECK_FALSE(f.model->read_console_references("scene.entity",version,0));
    auto fresh=f.model->read_console_references("scene.entity",f.model->console_reference_version(),0).value();
    CHECK(std::ranges::none_of(fresh.records,[&](const auto& r){return r.id==deleted;}));
    // Asset session replacement can change identities without changing scene revision.
    if(std::ranges::any_of(f.catalog.commands,[](const auto& d){return d.name=="assets.open";})) {
        REQUIRE(f.model->execute_console("assets.open manifest=project.json"));
        version=f.model->console_reference_version();
        auto active=f.model->read_console_references("assets.any",version,0); REQUIRE(active); CHECK(active->records.size()==2);
        f.files.write("empty.json",R"({"format":"DeckerProject","version":1,"name":"Empty catalog","scene":"scene.json","assets":[]})");
        auto assets=Json{{"method","assets.open"},{"params",{{"manifest","empty.json"},{"guard",{{"session_id",version.assets.session_id},{"revision",version.assets.revision}}}}}};
        REQUIRE(f.model->execute_console(assets.dump()));
        const auto next=f.model->console_reference_version();
        CHECK(next.revision==version.revision); CHECK(next.assets.session_id!=version.assets.session_id);
        CHECK_FALSE(f.model->read_console_references("assets.any",version,0));
        auto page=f.model->read_console_references("assets.any",next,0); REQUIRE(page);
        CHECK(page->records.empty()); CHECK_FALSE(page->has_more);
        // entity.set_assets uses the scene's project, not the unrelated active catalog.
        auto references=f.model->read_console_references("assets.reference",next,0); REQUIRE(references); CHECK(references->records.size()==2);
    }
}

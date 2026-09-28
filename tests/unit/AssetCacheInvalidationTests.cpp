#include "TextureTestSupport.hpp"
#include "AssetCacheInternal.hpp"
#include <catch2/generators/catch_generators.hpp>

namespace {
using namespace dk::asset_detail;
namespace fs = std::filesystem;
using Json = nlohmann::json;
fs::path at(const GltfFixture& f, std::string_view name) { return f.files.root / *dk::path_from_utf8(name); }
std::string index_path(dk::AssetId id) { return std::string{cache_root}+"/current/"+id.to_string()+".json"; }
std::string read(const fs::path& path) { return read_cache_text(path,cache_entry_limit); }
dk::CachedAsset compile(GltfFixture& f, double scale=1) {
    auto result=dk::compile_cached_asset(f.paths,{"assets/模型.gltf",scale});
    if(!result) { INFO(result.error().message); }
    REQUIRE(result); return std::move(*result);
}
}
TEST_CASE("cache invalidates byte changes despite identical size and mtime and preserves identity", "[cache]")
{
    const auto scenario=GENERATE(0,1,2,3); GltfFixture f; textured(f); f.save();
    const auto first=compile(f); const auto meta=read(at(f,"assets/模型.gltf.meta"));
    if(scenario==0) {
        const auto bin=at(f,"assets/数据.bin"); const auto time=fs::last_write_time(bin);
        f.binary[0]=std::byte{0}; f.binary[1]=std::byte{0}; f.binary[2]=std::byte{0}; f.binary[3]=std::byte{0x41};
        REQUIRE(dk::write_file_bytes(bin,f.binary)); fs::last_write_time(bin,time);
    } else if(scenario==1) {
        REQUIRE(dk::write_file_bytes(at(f,"assets/贴图.png"),image_fixture("red.jpg")));
    } else if(scenario==2) {
        fs::rename(at(f,"assets/贴图.png"),at(f,"assets/新图.png"));
        f.json["images"][0]["uri"]="新图.png"; f.save();
    } else {
        f.json["materials"][0]["pbrMetallicRoughness"]["roughnessFactor"]=0.2; f.save();
    }
    const auto rebuilt=compile(f); REQUIRE_FALSE(rebuilt.cache_hit); REQUIRE(rebuilt.key!=first.key);
    REQUIRE(rebuilt.artifact.data.mesh.id==first.artifact.data.mesh.id);
    REQUIRE(rebuilt.artifact.data.textures[0].id==first.artifact.data.textures[0].id);
    REQUIRE(read(at(f,"assets/模型.gltf.meta"))==meta); REQUIRE_FALSE(rebuilt.miss_reason.empty());
    REQUIRE(dk::load_cpu_artifact(at(f,first.directory))); REQUIRE(compile(f).cache_hit);
    if(scenario==0) { REQUIRE(rebuilt.artifact.data.mesh.primitives[0].positions[0].x()==8); }
    if(scenario==1) { REQUIRE(std::to_integer<unsigned>(rebuilt.artifact.data.textures[0].rgba8[4])>=250); }
    if(scenario==2) { REQUIRE(rebuilt.artifact.inputs.back().path=="assets/新图.png"); }
}
TEST_CASE("cache rebuilds missing corrupt or incompatible entries without replacing identities or unknown data", "[cache]")
{
    const auto scenario=GENERATE(0,1,2,3,4,5,6,7,8,9,10,11,12); GltfFixture f; textured(f); f.save();
    const auto first=compile(f); const auto root=first.artifact.data.mesh.id;
    const auto directory=std::string{first.directory};
    const auto meta=read(at(f,"assets/模型.gltf.meta"));
    auto entry=Json::parse(read(at(f,directory+"/entry.json")));
    switch(scenario) {
    case 0: f.files.write(*dk::path_from_utf8(directory+"/data.bin"),"bad data"); break;
    case 1: f.files.write(*dk::path_from_utf8(directory+"/manifest.json"),"bad manifest"); break;
    case 2: entry["version"]=2; break;
    case 3: entry["algorithm"]="unknown-hash"; break;
    case 4: entry["descriptor"]["implementation_version"]=2; break;
    case 5: { auto j=Json::parse(read(at(f,index_path(root)))); j["version"]=2; f.files.write(*dk::path_from_utf8(index_path(root)),j.dump()); break; }
    case 6: fs::remove(at(f,directory+"/data.bin")); break;
    case 7: fs::remove_all(at(f,cache_root)); break; // This fixture owns a unique project tree; meta is outside it.
    case 8: entry["descriptor"]["inputs"][0]["digest"]=std::string(32,'0'); break;
    case 9: f.files.write(*dk::path_from_utf8(directory+"/entry.json"),"bad entry"); break;
    case 10: { auto j=Json::parse(read(at(f,index_path(root)))); j["build"]="../escape"; f.files.write(*dk::path_from_utf8(index_path(root)),j.dump()); break; }
    case 11: fs::remove(at(f,index_path(root))); break;
    case 12: {
        auto j=Json::parse(read(at(f,directory+"/manifest.json"))); j["version"]=2; const auto text=j.dump();
        f.files.write(*dk::path_from_utf8(directory+"/manifest.json"),text);
        entry["manifest_digest"]=content_digest(std::as_bytes(std::span{text.data(),text.size()})).hex(); break;
    }
    }
    if(scenario==2||scenario==3||scenario==4||scenario==8||scenario==12) { f.files.write(*dk::path_from_utf8(directory+"/entry.json"),entry.dump()); }
    const auto old_entry=optional_cache_text(at(f,directory+"/entry.json"),cache_entry_limit);
    const auto rebuilt=compile(f); REQUIRE_FALSE(rebuilt.cache_hit); REQUIRE(rebuilt.key==first.key);
    REQUIRE(rebuilt.directory!=first.directory); REQUIRE(rebuilt.artifact.data.mesh.id==root);
    REQUIRE(read(at(f,"assets/模型.gltf.meta"))==meta); REQUIRE_FALSE(rebuilt.miss_reason.empty());
    REQUIRE(optional_cache_text(at(f,directory+"/entry.json"),cache_entry_limit)==old_entry);
    REQUIRE(compile(f).cache_hit);
}
TEST_CASE("cache failure from missing source dependency or malformed metadata keeps old index", "[cache]")
{
    const auto scenario=GENERATE(0,1,2,3,4); GltfFixture f; textured(f); f.save();
    const auto first=compile(f); const auto index=read(at(f,index_path(first.artifact.data.mesh.id)));
    switch(scenario) {
    case 0: fs::remove(at(f,"assets/贴图.png")); break;
    case 1: fs::remove(at(f,"assets/模型.gltf")); break;
    case 2: f.files.write(*dk::path_from_utf8("assets/贴图.png"),"broken image"); break;
    case 3: f.files.write(*dk::path_from_utf8("assets/模型.gltf.meta"),"unknown meta"); break;
    case 4: f.files.write(".decker/asset-operations/pending.json","unknown operation"); break;
    }
    const auto meta=read(at(f,"assets/模型.gltf.meta"));
    REQUIRE_FALSE(dk::compile_cached_asset(f.paths,{"assets/模型.gltf"}));
    REQUIRE(read(at(f,index_path(first.artifact.data.mesh.id)))==index);
    REQUIRE(read(at(f,"assets/模型.gltf.meta"))==meta); REQUIRE(dk::load_cpu_artifact(at(f,first.directory)));
}
TEST_CASE("cache cleanup deletes only verified unreferenced builds and retained CPU values stay alive", "[cache]")
{
    GltfFixture f; textured(f); f.save();
    const auto old=compile(f); const auto latest=compile(f,2);
    const auto meta=read(at(f,"assets/模型.gltf.meta")); const auto source=read(at(f,"assets/模型.gltf"));
    f.files.write("project.json","unrelated project");
    f.files.write(std::string{cache_root}+"/entries/unknown/keep.txt","unknown");
    f.files.write(std::string{cache_root}+"/tmp/orphan/keep.txt","temporary");
    const auto cleaned=dk::clean_asset_cache(f.paths); REQUIRE(cleaned);
    REQUIRE(cleaned->removed==1); REQUIRE(cleaned->retained==1); REQUIRE(cleaned->skipped==1); REQUIRE(cleaned->failed==0);
    REQUIRE_FALSE(fs::exists(at(f,old.directory))); REQUIRE(dk::load_cpu_artifact(at(f,latest.directory)));
    REQUIRE(old.artifact.data.mesh.primitives[0].positions[0]==dk::Vec3f{1,2,3});
    REQUIRE(old.artifact.data.textures[0].rgba8[11]==std::byte{128});
    REQUIRE(read(at(f,"assets/模型.gltf.meta"))==meta); REQUIRE(read(at(f,"assets/模型.gltf"))==source);
    REQUIRE(read(at(f,"project.json"))=="unrelated project");
    REQUIRE(read(at(f,std::string{cache_root}+"/tmp/orphan/keep.txt"))=="temporary");
    REQUIRE(compile(f,2).cache_hit); REQUIRE(dk::clean_asset_cache(f.paths)->removed==0);
}
TEST_CASE("cache cleanup refuses bad indexes budgets pending operations and current changes before deletion", "[cache]")
{
    const auto scenario=GENERATE(0,1,2,3); GltfFixture f; textured(f); f.save();
    const auto old=compile(f); const auto latest=compile(f,2); const auto name=index_path(latest.artifact.data.mesh.id);
    if(scenario==0) { f.files.write(*dk::path_from_utf8(name),"unknown index"); }
    if(scenario==2) { f.files.write(".decker/asset-operations/foreign","pending"); }
    CacheHook hook=[&](auto step)->dk::Result<void>{ if(step==CacheStep::clean && scenario==3) { f.files.write(*dk::path_from_utf8(name),"concurrent current"); } return {}; };
    ScopedCacheHook scope{hook};
    REQUIRE_FALSE(dk::clean_asset_cache(f.paths,scenario==1 ? 1 : 10000));
    REQUIRE(dk::load_cpu_artifact(at(f,old.directory))); REQUIRE(dk::load_cpu_artifact(at(f,latest.directory)));
    if(scenario==2) { REQUIRE(read(at(f,".decker/asset-operations/foreign"))=="pending"); }
}
TEST_CASE("cache cleanup preserves extra corrupt and concurrently modified entry files", "[cache]")
{
    const auto scenario=GENERATE(0,1,2,3); GltfFixture f; textured(f); f.save();
    const auto old=compile(f); const auto latest=compile(f,2); const auto name=std::string{old.directory};
    if(scenario==0) { f.files.write(*dk::path_from_utf8(name+"/foreign.txt"),"foreign"); }
    if(scenario==1) { f.files.write(*dk::path_from_utf8(name+"/data.bin"),"changed"); }
    if(scenario==2) { auto j=Json::parse(read(at(f,name+"/entry.json"))); j["version"]=99; f.files.write(*dk::path_from_utf8(name+"/entry.json"),j.dump()); }
    CacheHook hook=[&](auto step)->dk::Result<void>{ if(step==CacheStep::clean && scenario==3) { f.files.write(*dk::path_from_utf8(name+"/data.bin"),"changed later"); } return {}; };
    ScopedCacheHook scope{hook}; const auto cleaned=dk::clean_asset_cache(f.paths); REQUIRE(cleaned);
    REQUIRE(cleaned->removed==0); REQUIRE(cleaned->skipped==1); REQUIRE(cleaned->retained==1);
    REQUIRE(fs::exists(at(f,name+"/entry.json"))); REQUIRE(dk::load_cpu_artifact(at(f,latest.directory)));
    if(scenario==3) { REQUIRE(read(at(f,name+"/data.bin"))=="changed later"); }
}
TEST_CASE("cache cleanup reports partial IO failure while protecting current and identities", "[cache]")
{
    GltfFixture f; textured(f); f.save(); const auto old=compile(f); const auto latest=compile(f,2);
    const auto meta=read(at(f,"assets/模型.gltf.meta")); const auto before=read(at(f,index_path(latest.artifact.data.mesh.id)));
    unsigned calls=0;
    CacheHook hook=[&](auto step)->dk::Result<void>{
        if(step==CacheStep::clean_file && ++calls==2) { return std::unexpected(dk::Error{dk::ErrorCode::io_error,"injected delete"}); } return {};
    };
    ScopedCacheHook scope{hook}; const auto result=dk::clean_asset_cache(f.paths); REQUIRE(result);
    REQUIRE(result->failed==1); REQUIRE(result->removed==0); REQUIRE_FALSE(result->diagnostics.empty());
    REQUIRE_FALSE(fs::exists(at(f,std::string{old.directory}+"/data.bin")));
    REQUIRE(fs::exists(at(f,std::string{old.directory}+"/entry.json")));
    REQUIRE(read(at(f,"assets/模型.gltf.meta"))==meta); REQUIRE(read(at(f,index_path(latest.artifact.data.mesh.id)))==before);
    REQUIRE(compile(f,2).cache_hit);
}
TEST_CASE("cache cleanup preserves hard linked entry files and all outside data", "[cache]")
{
    GltfFixture f; textured(f); f.save(); const auto old=compile(f); const auto latest=compile(f,2);
    const auto data=at(f,std::string{old.directory}+"/data.bin"); const auto outside=f.files.root/"outside.bin";
    const auto before=read(data); std::error_code error; fs::create_hard_link(data,outside,error); REQUIRE_FALSE(error);
    const auto cleaned=dk::clean_asset_cache(f.paths); REQUIRE(cleaned);
    REQUIRE(cleaned->removed==0); REQUIRE(cleaned->skipped==1); REQUIRE(cleaned->retained==1);
    REQUIRE(read(data)==before); REQUIRE(read(outside)==before); REQUIRE(dk::load_cpu_artifact(at(f,latest.directory)));
}

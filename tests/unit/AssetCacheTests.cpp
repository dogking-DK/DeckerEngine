#include "TextureTestSupport.hpp"
#include "AssetCacheInternal.hpp"
#include <catch2/generators/catch_generators.hpp>

namespace {
namespace fs = std::filesystem;
using namespace dk::asset_detail;
fs::path path(const GltfFixture& f, std::string_view s) { return f.files.root / *dk::path_from_utf8(s); }
std::string current(const GltfFixture& f, dk::AssetId id)
{ return read_cache_text(path(f,std::string{cache_root}+"/current/"+id.to_string()+".json"),cache_index_limit); }
void no_temporary(const GltfFixture& f)
{
    const auto directory=path(f,std::string{cache_root}+"/tmp");
    if(fs::exists(directory)) { REQUIRE(fs::is_empty(directory)); }
}
}
TEST_CASE("cache key is canonical and every content contract identity or setting contributes", "[cache]")
{
    GltfFixture f; textured(f); f.save();
    CacheDescriptor golden; golden.source="a.gltf"; golden.metadata.unit_scale=1.25;
    golden.metadata.root_id=dk::AssetId::parse("11111111-1111-4111-8111-111111111111").value();
    golden.metadata.outputs.push_back({owned("mesh/0"),golden.metadata.root_id,dk::AssetKind::mesh});
    golden.inputs.push_back({owned("a.gltf"),owned("0123456789abcdef0123456789abcdef"),3});
    // Independent Python struct encoder produced 348 bytes, then XXH3 canonical big endian.
    REQUIRE(cache_key(golden)->hex()=="de7e9b8a3a75208f1f16f3f5b6a9466e");
    auto first=dk::compile_cached_asset(f.paths,{"assets/模型.gltf"}); REQUIRE(first);
    auto base=describe_cache(first->artifact); const auto original=cache_key(base); REQUIRE(original);
    REQUIRE(original->hex()==std::string_view{first->key});
    auto reordered=base; std::reverse(reordered.inputs.begin(),reordered.inputs.end());
    std::reverse(reordered.metadata.outputs.begin(),reordered.metadata.outputs.end());
    REQUIRE(cache_key(reordered).value()==*original);
    for(unsigned scenario=0;scenario<13;++scenario) {
        auto changed=base;
        switch(scenario) {
        case 0: changed.versions.algorithm="other"; break;
        case 1: changed.versions.importer="other"; break;
        case 2: ++changed.versions.cache; break;
        case 3: ++changed.versions.artifact; break;
        case 4: ++changed.versions.implementation; break;
        case 5: ++changed.versions.contract; break;
        case 6: changed.metadata.unit_scale=2; break;
        case 7: changed.inputs[0].digest=owned(std::string(32,'0')); break;
        case 8: changed.inputs[1].digest=owned(std::string(32,'0')); break;
        case 9: ++changed.inputs[1].bytes; break;
        case 10: changed.inputs[1].path="assets/another.bin"; break;
        case 11: changed.metadata.outputs[0].id=dk::AssetId::generate().value(); break;
        case 12: changed.source="assets/renamed.gltf"; changed.inputs[0].path=owned(changed.source); break;
        }
        INFO(scenario); const auto key=cache_key(changed); REQUIRE(key); REQUIRE(*key!=*original);
    }
    auto a=base,b=base; a.versions.algorithm="ab"; a.versions.importer="c"; b.versions.algorithm="a"; b.versions.importer="bc";
    REQUIRE(cache_key(a).value()!=cache_key(b).value());
    base.inputs.push_back(base.inputs.front()); REQUIRE_FALSE(cache_key(base));
}
TEST_CASE("cache hit skips importing and preserves current metadata and owning CPU data", "[cache]")
{
    GltfFixture f; textured(f); f.save();
    const auto first=dk::compile_cached_asset(f.paths,{"assets/模型.gltf",2}); REQUIRE(first); REQUIRE_FALSE(first->cache_hit);
    REQUIRE(first->artifact.inputs.size()==3); REQUIRE(first->artifact.data.textures.size()==1);
    const auto root=first->artifact.data.mesh.id;
    const auto meta=path(f,"assets/模型.gltf.meta"), index=path(f,std::string{cache_root}+"/current/"+root.to_string()+".json");
    const auto meta_time=fs::last_write_time(meta), index_time=fs::last_write_time(index);
    CacheHook hook=[](auto step)->dk::Result<void>{
        if(step==CacheStep::import) { return std::unexpected(dk::Error{dk::ErrorCode::internal_error,"cache hit decoded source"}); } return {};
    };
    ScopedCacheHook scope{hook}; const auto hit=dk::compile_cached_asset(f.paths,{"assets/模型.gltf"}); REQUIRE(hit); REQUIRE(hit->cache_hit);
    REQUIRE(hit->key==first->key); REQUIRE(hit->directory==first->directory); REQUIRE(hit->artifact.data.mesh.id==root);
    REQUIRE(hit->artifact.data.mesh.primitives[0].positions[0]==dk::Vec3f{2,4,6});
    REQUIRE(fs::last_write_time(meta)==meta_time); REQUIRE(fs::last_write_time(index)==index_time);
    REQUIRE(nlohmann::json::parse(hit->summary_json)["cache_hit"]==true); no_temporary(f);
}
TEST_CASE("cache publication failures keep previous index and expose identity commit boundary", "[cache]")
{
    const auto at=GENERATE(CacheStep::validate_inputs,CacheStep::publish,CacheStep::metadata,CacheStep::current);
    const bool existing=GENERATE(false,true); GltfFixture f; textured(f); f.save();
    std::optional<dk::CachedAsset> first;
    if(existing) { first=dk::compile_cached_asset(f.paths,{"assets/模型.gltf"}).value(); }
    const auto before_index=existing ? current(f,first->artifact.data.mesh.id) : "";
    const auto meta=path(f,"assets/模型.gltf.meta"); const auto before_meta=optional_cache_text(meta,dk::asset_meta_byte_limit);
    {
        CacheHook hook=[&](auto step)->dk::Result<void>{
            if(step==at) { return std::unexpected(dk::Error{dk::ErrorCode::io_error,"injected cache failure"}); } return {};
        };
        ScopedCacheHook scope{hook}; const auto failed=dk::compile_cached_asset(f.paths,{"assets/模型.gltf",2}); REQUIRE_FALSE(failed);
        if(at==CacheStep::current) {
            REQUIRE(std::any_of(failed.error().context.begin(),failed.error().context.end(),[](const auto& s){return s.find("identity committed")!=s.npos;}));
            REQUIRE(dk::parse_asset_meta(read_cache_text(meta,dk::asset_meta_byte_limit))->unit_scale==2);
        } else { REQUIRE(optional_cache_text(meta,dk::asset_meta_byte_limit)==before_meta); }
    }
    if(existing) { REQUIRE(current(f,first->artifact.data.mesh.id)==before_index); REQUIRE(dk::load_cpu_artifact(path(f,first->directory))); }
    else if(at==CacheStep::current) {
        const auto id=dk::parse_asset_meta(read_cache_text(meta,dk::asset_meta_byte_limit))->root_id;
        REQUIRE_FALSE(fs::exists(path(f,std::string{cache_root}+"/current/"+id.to_string()+".json")));
    }
    no_temporary(f);
    const auto retry=dk::compile_cached_asset(f.paths,{"assets/模型.gltf",2}); REQUIRE(retry);
    if(existing) { REQUIRE(retry->artifact.data.mesh.id==first->artifact.data.mesh.id); }
    REQUIRE(dk::compile_cached_asset(f.paths,{"assets/模型.gltf"})->cache_hit);
}
TEST_CASE("cache refuses changing snapshots and foreign current during publication", "[cache]")
{
    const bool input=GENERATE(false,true); GltfFixture f; textured(f); f.save();
    auto first=dk::compile_cached_asset(f.paths,{"assets/模型.gltf"}).value(); const auto before=current(f,first.artifact.data.mesh.id);
    const auto meta=read_cache_text(path(f,"assets/模型.gltf.meta"),dk::asset_meta_byte_limit);
    CacheHook hook=[&](auto step)->dk::Result<void>{
        if(step==CacheStep::validate_inputs) {
            if(input) { f.files.write(*dk::path_from_utf8("assets/贴图.png"),"changed"); }
            else { f.files.write(*dk::path_from_utf8(std::string{cache_root}+"/current/"+first.artifact.data.mesh.id.to_string()+".json"),"foreign index"); }
        } return {};
    };
    ScopedCacheHook scope{hook}; const auto failed=dk::compile_cached_asset(f.paths,{"assets/模型.gltf",2}); REQUIRE_FALSE(failed);
    REQUIRE(failed.error().code==dk::ErrorCode::conflict);
    REQUIRE(current(f,first.artifact.data.mesh.id)==(input ? before : "foreign index"));
    REQUIRE(read_cache_text(path(f,"assets/模型.gltf.meta"),dk::asset_meta_byte_limit)==meta); no_temporary(f);
}

#include "TextureTestSupport.hpp"
#include "CpuArtifactInternal.hpp"
#include "ContentDigest.hpp"
#include <dk/assets/AssetCompiler.hpp>
#include <catch2/generators/catch_generators.hpp>

namespace {
using J = nlohmann::json;
namespace fs = std::filesystem;
auto asset_path(const GltfFixture& f, std::string_view path) { return f.files.root / *dk::path_from_utf8(path); }
std::string read(const fs::path& path)
{
    const auto bytes = dk::read_file_bytes(path).value();
    return {reinterpret_cast<const char*>(bytes.data()),bytes.size()};
}
void no_staging(const GltfFixture& f)
{
    for(const auto& entry : fs::directory_iterator(f.files.root)) {
        REQUIRE_FALSE(entry.path().filename().string().starts_with(".dk-asset-"));
    }
}
}
TEST_CASE("compiler publishes portable CPU artifact and preserves meta identity on reimport", "[pipeline]")
{
    GltfFixture f; textured(f); f.save();
    auto first=dk::compile_asset(f.paths,{"assets/模型.gltf","产物一",2});
    if(!first) { INFO(first.error().message); }
    REQUIRE(first); REQUIRE(first->outputs.size()==3);
    const auto meta=asset_path(f,"assets/模型.gltf.meta"); const auto original=read(meta); const auto time=fs::last_write_time(meta);
    const auto data_path=asset_path(f,"产物一/data.bin"); const auto first_data=read(data_path);
    REQUIRE(first_data.size()==124); // 36 position + 36 normal + 24 UV + 12 index + 16 RGBA.
    REQUIRE(static_cast<unsigned char>(first_data[2])==0); REQUIRE(static_cast<unsigned char>(first_data[3])==0x40); // float32 LE 2.
    auto loaded=dk::load_cpu_artifact(asset_path(f,"产物一")); REQUIRE(loaded);
    REQUIRE(loaded->data.mesh.id==first->root_id); REQUIRE(loaded->data.mesh.primitives[0].positions[0]==dk::Vec3f{2,4,6});
    REQUIRE(loaded->data.mesh.primitives[0].material==loaded->data.materials[0].id);
    REQUIRE(loaded->data.materials[0].base_color_texture==loaded->data.textures[0].id);
    REQUIRE(loaded->data.textures[0].rgba8[11]==std::byte{128}); REQUIRE(loaded->data.unit_scale==2);
    REQUIRE(loaded->inputs.size()==3); REQUIRE(loaded->inputs[2].path=="assets/贴图.png");
    const auto summary=J::parse(first->summary_json); REQUIRE(summary["root_id"]==first->root_id.to_string()); REQUIRE(summary["outputs"].size()==3);
    // Preserve legal preexisting formatting on a semantic no-op, too.
    f.files.write(*dk::path_from_utf8("assets/模型.gltf.meta"),J::parse(original).dump());
    const auto compact=read(meta); const auto compact_time=fs::last_write_time(meta);
    auto second=dk::compile_asset(f.paths,{"assets/模型.gltf","产物二"}); REQUIRE(second); REQUIRE(second->root_id==first->root_id);
    REQUIRE(read(meta)==compact); REQUIRE(fs::last_write_time(meta)==compact_time); REQUIRE(read(asset_path(f,"产物二/data.bin"))==first_data);
    REQUIRE(dk::load_cpu_artifact(asset_path(f,"产物二"))->data.textures[0].id==loaded->data.textures[0].id);
    auto scaled=dk::compile_asset(f.paths,{"assets/模型.gltf","产物三",3}); REQUIRE(scaled); REQUIRE(scaled->root_id==first->root_id);
    REQUIRE(dk::parse_asset_meta(read(meta))->unit_scale==3); REQUIRE(read(data_path)==first_data);
    REQUIRE(time<=fs::last_write_time(meta)); no_staging(f);
}
TEST_CASE("compiler failures preserve previous metadata and products", "[pipeline]")
{
    const auto scenario=GENERATE(0,1,2,3,4,5,6,7,8,9); GltfFixture f; textured(f); f.save();
    REQUIRE(dk::compile_asset(f.paths,{"assets/模型.gltf","old"}));
    const auto meta=asset_path(f,"assets/模型.gltf.meta"); const auto before=read(meta);
    const auto product=read(asset_path(f,"old/data.bin"));
    dk::AssetCompileRequest request{"assets/模型.gltf","next"};
    switch(scenario) {
    case 0: f.files.write(*dk::path_from_utf8("assets/贴图.png"),"bad PNG"); break;
    case 1: request.output_directory="old"; break;
    case 2: request.output_directory="../escape"; break;
    case 3: request.output_directory=".decker/output"; break;
    case 4: request.output_directory="assets"; break;
    case 5: request.output_directory="missing/child"; break;
    case 6: f.json["materials"][0]["pbrMetallicRoughness"].erase("baseColorTexture"); f.save(); break;
    case 7: f.files.write(".decker/asset-operations/unknown.json","pending"); break;
    case 8: request.unit_scale=0; break;
    case 9: request.limits.image_bytes=1; break;
    }
    REQUIRE_FALSE(dk::compile_asset(f.paths,request)); REQUIRE(read(meta)==before); REQUIRE(read(asset_path(f,"old/data.bin"))==product);
    REQUIRE_FALSE(fs::exists(f.files.root/"next")); no_staging(f);
}
TEST_CASE("compiler rejects invalid old meta and failed first imports without new identity", "[pipeline]")
{
    const bool old=GENERATE(false,true); GltfFixture f; textured(f); f.save();
    if(old) { f.files.write(*dk::path_from_utf8("assets/模型.gltf.meta"),"unknown metadata"); }
    else { f.files.write(*dk::path_from_utf8("assets/贴图.png"),"broken"); }
    REQUIRE_FALSE(dk::compile_asset(f.paths,{"assets/模型.gltf","out"}));
    REQUIRE_FALSE(fs::exists(f.files.root/"out")); no_staging(f);
    if(old) { REQUIRE(read(asset_path(f,"assets/模型.gltf.meta"))=="unknown metadata"); }
    else { REQUIRE_FALSE(fs::exists(asset_path(f,"assets/模型.gltf.meta"))); }
}
TEST_CASE("compiler rolls back injected IO failures at each publication boundary", "[pipeline]")
{
    const auto at=GENERATE(dk::asset_detail::CompileStep::validate_inputs,dk::asset_detail::CompileStep::publish,dk::asset_detail::CompileStep::metadata);
    const bool existing=GENERATE(false,true); GltfFixture f; textured(f); f.save();
    if(existing) { REQUIRE(dk::compile_asset(f.paths,{"assets/模型.gltf","old"})); }
    const auto meta=asset_path(f,"assets/模型.gltf.meta"); const auto before=existing ? read(meta) : std::string{};
    dk::asset_detail::CompileHook hook=[&](auto step)->dk::Result<void>{
        if(step==at) { return std::unexpected(dk::Error{dk::ErrorCode::io_error,"injected"}); } return {};
    };
    dk::asset_detail::ScopedCompileHook scope{hook};
    const auto result=dk::compile_asset(f.paths,{"assets/模型.gltf","next",2}); REQUIRE_FALSE(result);
    REQUIRE(result.error().code==dk::ErrorCode::io_error);
    REQUIRE(fs::exists(meta)==existing); if(existing) { REQUIRE(read(meta)==before); REQUIRE(dk::load_cpu_artifact(f.files.root/"old")); }
    REQUIRE_FALSE(fs::exists(f.files.root/"next")); no_staging(f);
}
TEST_CASE("compiler detects changed inputs metadata and destination before commit", "[pipeline]")
{
    const auto scenario=GENERATE(0,1,2,3); GltfFixture f; textured(f); f.save();
    REQUIRE(dk::compile_asset(f.paths,{"assets/模型.gltf","old"})); const auto meta=asset_path(f,"assets/模型.gltf.meta"); const auto original=read(meta);
    dk::asset_detail::CompileHook hook=[&](auto step)->dk::Result<void>{
        if(step==dk::asset_detail::CompileStep::validate_inputs && scenario==0) { f.files.write(*dk::path_from_utf8("assets/贴图.png"),"changed"); }
        if(step==dk::asset_detail::CompileStep::validate_inputs && scenario==1) { f.files.write(*dk::path_from_utf8("assets/模型.gltf.meta"),"concurrent meta"); }
        if(step==dk::asset_detail::CompileStep::publish && scenario==2) { f.files.write("next/foreign","keep"); }
        if(step==dk::asset_detail::CompileStep::metadata && scenario==3) { f.files.write(*dk::path_from_utf8("assets/模型.gltf.meta"),"concurrent meta"); }
        return {};
    };
    dk::asset_detail::ScopedCompileHook scope{hook}; const auto result=dk::compile_asset(f.paths,{"assets/模型.gltf","next",2}); REQUIRE_FALSE(result);
    REQUIRE(read(meta)==(scenario==1||scenario==3 ? "concurrent meta" : original));
    if(scenario==2) { REQUIRE(read(f.files.root/"next/foreign")=="keep"); }
    else { REQUIRE_FALSE(fs::exists(f.files.root/"next")); }
    REQUIRE(dk::load_cpu_artifact(f.files.root/"old")); no_staging(f);
}
TEST_CASE("compiler cleanup preserves foreign files and reports incomplete cleanup", "[pipeline]")
{
    const bool changed=GENERATE(false,true); GltfFixture f; textured(f); f.save();
    dk::asset_detail::CompileHook hook=[&](auto step)->dk::Result<void>{
        if(step==dk::asset_detail::CompileStep::metadata) {
            f.files.write(changed ? "next/data.bin" : "next/foreign","keep"); return std::unexpected(dk::Error{dk::ErrorCode::io_error,"injected"});
        } return {};
    };
    dk::asset_detail::ScopedCompileHook scope{hook}; const auto result=dk::compile_asset(f.paths,{"assets/模型.gltf","next"}); REQUIRE_FALSE(result);
    REQUIRE(read(f.files.root/(changed ? "next/data.bin" : "next/foreign"))=="keep");
    REQUIRE(std::any_of(result.error().context.begin(),result.error().context.end(),[](const auto& s){return s.find("Cleanup incomplete")!=s.npos;}));
    REQUIRE_FALSE(fs::exists(asset_path(f,"assets/模型.gltf.meta")));
}
TEST_CASE("artifact reader rejects corrupt manifests data ranges identities and references", "[pipeline]")
{
    const auto scenario=GENERATE(0,1,2,3,4,5,6,7,8,9,10,11,12,13); GltfFixture f; textured(f); f.save();
    REQUIRE(dk::compile_asset(f.paths,{"assets/模型.gltf","out"})); const auto meta=read(asset_path(f,"assets/模型.gltf.meta"));
    auto manifest=J::parse(read(f.files.root/"out/manifest.json")); auto bytes=dk::read_file_bytes(f.files.root/"out/data.bin").value();
    switch(scenario) {
    case 0: manifest["version"]=2; break;
    case 1: manifest["hash_algorithm"]="unknown"; break;
    case 2: bytes[0]^=std::byte{1}; break;
    case 3: manifest["primitives"][0]["positions"]["offset"]=1; break;
    case 4: manifest["primitives"][0]["positions"]["count"]=99999999999ULL; break;
    case 5: manifest["primitives"][0]["bounds_min"][0]=999; break;
    case 6: manifest["materials"][0]["base_color_texture"]=dk::AssetId::generate()->to_string(); break;
    case 7: manifest["textures"][0]["rgba8"]["count"]=15; break;
    case 8: manifest["textures"][0]["sampler"]["wrap_s"]=0; break;
    case 9: manifest["inputs"][0]["path"]="../escape"; break;
    case 10: bytes[96]=std::byte{99}; manifest["data"]["digest"]=dk::asset_detail::content_digest(bytes).hex(); break;
    case 11: bytes[2]=std::byte{0x80}; bytes[3]=std::byte{0x7f}; manifest["data"]["digest"]=dk::asset_detail::content_digest(bytes).hex(); break;
    case 12: manifest["textures"]=J::array(); break;
    case 13: manifest["data"]["file"]="../elsewhere"; break;
    }
    f.files.write("out/manifest.json",manifest.dump()); REQUIRE(dk::write_file_bytes(f.files.root/"out/data.bin",bytes));
    const auto result=dk::load_cpu_artifact(f.files.root/"out"); REQUIRE_FALSE(result);
    REQUIRE(read(asset_path(f,"assets/模型.gltf.meta"))==meta); REQUIRE(read(f.files.root/"out/manifest.json")==manifest.dump());
}

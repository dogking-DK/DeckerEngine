#include "GltfTestSupport.hpp"
#include <catch2/generators/catch_generators.hpp>

TEST_CASE("gltf imports interleaved mesh primitives material factors and local bounds", "[import]")
{
    GltfFixture f; f.json["meshes"][0]["primitives"].push_back(f.json["meshes"][0]["primitives"][0]); f.save();
    auto result = dk::import_gltf(f.paths,{"assets/模型.gltf",{},2}); REQUIRE(result);
    REQUIRE(result->mesh.primitives.size() == 2); REQUIRE(result->materials.size() == 1); REQUIRE(result->outputs.size() == 2);
    const auto& p = result->mesh.primitives[0]; REQUIRE(p.positions[0] == dk::Vec3f{2,4,6});
    REQUIRE(p.bounds_min == dk::Vec3f{-4,0,-2}); REQUIRE(p.bounds_max == dk::Vec3f{2,8,6});
    REQUIRE(p.normals[1] == dk::Vec3f{0,0,1}); REQUIRE(p.texcoords[2] == dk::Vec2f{0,1});
    REQUIRE(p.indices == dk::Vector<std::uint32_t>{0,1,2}); REQUIRE(p.material == result->materials[0].id);
    const auto& m = result->materials[0]; REQUIRE(m.base_color.isApprox(dk::Vec4f{0.2f,0.4f,0.6f,0.8f}));
    REQUIRE(m.metallic == 0.3f); REQUIRE(m.roughness == 0.7f); REQUIRE(m.alpha_mode == dk::AlphaMode::mask); REQUIRE(m.double_sided);
    REQUIRE(result->inputs.size() == 2); REQUIRE(result->inputs[1].path == "assets/数据.bin"); REQUIRE(result->diagnostics.size() == 1);
    auto repeated = dk::import_gltf(f.paths,{"assets/模型.gltf",result->outputs,2}); REQUIRE(repeated);
    REQUIRE(repeated->mesh.id == result->mesh.id); REQUIRE(repeated->materials[0].id == m.id);
    REQUIRE_FALSE(std::filesystem::exists(f.files.root / *dk::path_from_utf8("assets/模型.gltf.meta")));
}
TEST_CASE("gltf handles GLB and unsigned index widths or unindexed defaults", "[import]")
{
    const auto type = GENERATE(0,5121,5123,5125); GltfFixture f;
    auto& primitive = f.json["meshes"][0]["primitives"][0]; primitive.erase("material"); primitive["attributes"].erase("NORMAL"); primitive["attributes"].erase("TEXCOORD_0");
    if (type == 0) { primitive.erase("indices"); }
    else {
        const auto width = type == 5121 ? 1U : type == 5123 ? 2U : 4U; f.binary.resize(96);
        for (unsigned index = 0; index < 3; ++index) { for (unsigned i = 0; i < width; ++i) { f.binary.push_back(static_cast<std::byte>((index >> (i * 8)) & 255)); } }
        f.json["accessors"][3]["componentType"] = type; f.json["bufferViews"][1]["byteLength"] = 3 * width; f.json["buffers"][0]["byteLength"] = f.binary.size();
    }
    f.glb(); const auto imported = dk::import_gltf(f.paths,{"assets/模型.glb"}); REQUIRE(imported);
    REQUIRE(imported->inputs.size() == 1); const auto& p = imported->mesh.primitives[0];
    REQUIRE(p.indices == dk::Vector<std::uint32_t>{0,1,2}); REQUIRE(p.normals.empty()); REQUIRE(p.texcoords.empty()); REQUIRE_FALSE(p.material);
}
TEST_CASE("gltf rejects malformed ranges indices nonfinite and unsupported features with location", "[import]")
{
    const auto scenario = GENERATE(0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15); GltfFixture f;
    switch (scenario) {
    case 0: f.binary.resize(16); break;
    case 1: f.json["bufferViews"][0]["byteStride"] = 8; break;
    case 2: f.json["accessors"][0]["byteOffset"] = 1000; break;
    case 3: f.json["accessors"][0]["count"] = 100000000000ULL; break;
    case 4: f.binary[100] = std::byte{99}; break;
    case 5: f.binary[2] = std::byte{0x80}; f.binary[3] = std::byte{0x7f}; break;
    case 6: f.json["meshes"][0]["primitives"][0]["mode"] = 1; break;
    case 7: f.json["extensionsUsed"] = {"KHR_materials_unlit"}; break;
    case 8: f.json["accessors"][0]["sparse"] = {}; break;
    case 9: f.json["meshes"][0]["primitives"][0]["attributes"]["COLOR_0"] = 0; break;
    case 10: f.json["meshes"].push_back(f.json["meshes"][0]); break;
    case 11: f.json["meshes"][0]["primitives"][0]["material"] = 100; break;
    case 12: f.json["materials"][0]["normalTexture"] = {{"index",0}}; break;
    case 13: f.json["materials"][0]["pbrMetallicRoughness"]["roughnessFactor"] = -1; break;
    case 14: f.json["accessors"][0]["normalized"] = true; break;
    case 15: f.json["bufferViews"][0]["buffer"] = 100; break;
    }
    f.save(); const auto result = dk::import_gltf(f.paths,{"assets/模型.gltf"}); REQUIRE_FALSE(result); REQUIRE_FALSE(result.error().context.empty());
    REQUIRE_FALSE(std::filesystem::exists(f.files.root / *dk::path_from_utf8("assets/模型.gltf.meta")));
}
TEST_CASE("gltf enforces all byte and element budgets before producing data", "[import]")
{
    const auto scenario = GENERATE(0,1,2,3,4,5,6); GltfFixture f; f.save(); dk::GltfImportRequest request{"assets/模型.gltf"};
    switch (scenario) {
    case 0: request.limits.source_bytes = 16; break;
    case 1: request.limits.dependency_bytes = 10; break;
    case 2: request.limits.input_bytes = 16; break;
    case 3: request.limits.output_bytes = 10; break;
    case 4: request.limits.vertices = 2; break;
    case 5: request.limits.indices = 2; break;
    case 6: request.unit_scale = 1e300; break;
    }
    REQUIRE_FALSE(dk::import_gltf(f.paths,request));
}
TEST_CASE("gltf rejects network data absolute and escaping URIs", "[import]")
{
    const auto uri = GENERATE("https://host/data.bin","data:application/octet-stream;base64,AA==","../../../outside.bin","%2e%2e/%2e%2e/outside.bin","%00.bin","%ff.bin","/absolute.bin","C%3a/file.bin","bad%xy.bin");
    GltfFixture f; f.json["buffers"][0]["uri"] = uri; f.save(); REQUIRE_FALSE(dk::import_gltf(f.paths,{"assets/模型.gltf"}));
}
TEST_CASE("gltf resolves encoded sibling dependencies and owns data beyond context lifetime", "[import]")
{
    std::optional<dk::ImportResult> retained;
    {
        GltfFixture f; f.json["buffers"][0]["uri"] = "../shared/%E6%95%B0%E6%8D%AE.bin"; f.save();
        std::filesystem::create_directories(f.files.root / "shared");
        REQUIRE(dk::write_file_bytes(f.files.root / *dk::path_from_utf8("shared/数据.bin"), f.binary));
        auto result = dk::import_gltf(f.paths,{"assets/模型.gltf"}); REQUIRE(result);
        REQUIRE(result->inputs[1].path == "shared/数据.bin"); retained.emplace(std::move(*result));
    }
    REQUIRE(retained->mesh.primitives[0].positions[1] == dk::Vec3f{-2,4,1}); REQUIRE(retained->inputs[1].bytes.size() == 108);
}
TEST_CASE("gltf handles missing files removed mappings and owning result lifetime", "[import]")
{
    GltfFixture f; f.save(); auto result = dk::import_gltf(f.paths,{"assets/模型.gltf"}); REQUIRE(result);
    result->outputs.push_back({"material/9",*dk::AssetId::generate(),dk::AssetKind::material});
    auto missing = dk::import_gltf(f.paths,{"assets/模型.gltf",result->outputs}); REQUIRE_FALSE(missing); REQUIRE(missing.error().code == dk::ErrorCode::not_found);
    REQUIRE(std::filesystem::remove(f.files.root / *dk::path_from_utf8("assets/数据.bin")));
    missing = dk::import_gltf(f.paths,{"assets/模型.gltf"}); REQUIRE_FALSE(missing); REQUIRE(missing.error().code == dk::ErrorCode::not_found);
    REQUIRE(result->mesh.primitives[0].positions[0] == dk::Vec3f{1,2,3});
}

#include "TextureTestSupport.hpp"
#include <catch2/generators/catch_generators.hpp>

TEST_CASE("textures decode external PNG and embedded JPEG with sampler and identities", "[import]")
{
    const bool embedded = GENERATE(false,true); GltfFixture f;
    textured(f,embedded ? "red.jpg" : "rgba.png",embedded);
    if (embedded) { f.glb(); } else { f.save(); }
    const auto source = embedded ? "assets/模型.glb" : "assets/模型.gltf";
    auto result = dk::import_gltf(f.paths,{source});
    if (!result) { INFO(result.error().message); }
    REQUIRE(result); REQUIRE(result->textures.size() == 1); REQUIRE(result->outputs.size() == 3);
    const auto& t = result->textures[0]; REQUIRE(t.width == 2); REQUIRE(t.height == 2); REQUIRE(t.rgba8.size() == 16);
    REQUIRE(t.sampler.min_filter == dk::TextureFilter::linear_mipmap_linear); REQUIRE(t.sampler.mag_filter == dk::TextureFilter::nearest);
    REQUIRE(t.sampler.wrap_s == dk::TextureWrap::clamp); REQUIRE(t.sampler.wrap_t == dk::TextureWrap::mirrored_repeat);
    REQUIRE(result->materials[0].base_color_texture == t.id);
    if (embedded) {
        REQUIRE(result->inputs.size() == 1); REQUIRE(t.origin == "assets/模型.glb#image/0");
        REQUIRE(std::to_integer<unsigned>(t.rgba8[0]) >= 250); REQUIRE(std::to_integer<unsigned>(t.rgba8[1]) <= 3);
    } else {
        REQUIRE(result->inputs.size() == 3); REQUIRE(t.origin == "assets/贴图.png");
        const std::array<unsigned,16> expected{255,0,0,255,0,255,0,255,0,0,255,128,255,255,255,255};
        for (std::size_t i=0;i<expected.size();++i) { REQUIRE(std::to_integer<unsigned>(t.rgba8[i]) == expected[i]); }
    }
    const auto repeated = dk::import_gltf(f.paths,{source,result->outputs}); REQUIRE(repeated); REQUIRE(repeated->textures[0].id == t.id);
}
TEST_CASE("textures reject malformed unsupported and over-budget images without writes", "[import]")
{
    const auto scenario = GENERATE(0,1,2,3,4,5,6,7,8,9,10,11); GltfFixture f;
    textured(f,scenario == 0 ? "gray16.png" : "rgba.png"); dk::GltfImportRequest request{"assets/模型.gltf"};
    switch (scenario) {
    case 0: break;
    case 1: f.files.write(*dk::path_from_utf8("assets/贴图.png"),"broken image"); break;
    case 2: { auto bytes=image_fixture("rgba.png"); bytes.resize(40); REQUIRE(dk::write_file_bytes(f.files.root / *dk::path_from_utf8("assets/贴图.png"),bytes)); break; }
    case 3: request.limits.image_dimension=1; break;
    case 4: request.limits.image_bytes=15; break;
    case 5: request.limits.texture_bytes=15; break;
    case 6: request.limits.output_bytes=123; break;
    case 7: f.json["images"][0]["mimeType"]="image/jpeg"; break;
    case 8: f.json["samplers"][0]["magFilter"]=9987; break;
    case 9: f.json["samplers"][0]["wrapS"]=0; break;
    case 10: f.json["meshes"][0]["primitives"][0]["attributes"].erase("TEXCOORD_0"); break;
    case 11: f.json["materials"][0]["pbrMetallicRoughness"]["baseColorTexture"]["texCoord"]=1; break;
    }
    f.save(); const auto result=dk::import_gltf(f.paths,request); REQUIRE_FALSE(result);
    REQUIRE_FALSE(std::filesystem::exists(f.files.root / *dk::path_from_utf8("assets/模型.gltf.meta")));
}
TEST_CASE("textures share one output per texture and budget distinct samplers separately", "[import]")
{
    GltfFixture f; textured(f); f.json["textures"][0].erase("sampler");
    f.json["materials"].push_back(f.json["materials"][0]); f.json["meshes"][0]["primitives"].push_back(f.json["meshes"][0]["primitives"][0]);
    f.json["meshes"][0]["primitives"][1]["material"]=1; f.save();
    auto same=dk::import_gltf(f.paths,{"assets/模型.gltf"}); REQUIRE(same); REQUIRE(same->textures.size()==1);
    REQUIRE_FALSE(same->textures[0].sampler.min_filter); REQUIRE_FALSE(same->textures[0].sampler.mag_filter);
    REQUIRE(same->textures[0].sampler.wrap_s == dk::TextureWrap::repeat);
    f.json["textures"].push_back({{"source",0},{"sampler",0}});
    f.json["materials"][1]["pbrMetallicRoughness"]["baseColorTexture"]["index"]=1; f.save();
    auto different=dk::import_gltf(f.paths,{"assets/模型.gltf"}); REQUIRE(different); REQUIRE(different->textures.size()==2);
    REQUIRE(different->textures[0].id != different->textures[1].id);
    REQUIRE(different->inputs.size()==3);
    dk::GltfImportRequest limited{"assets/模型.gltf"}; limited.limits.texture_bytes=31;
    REQUIRE_FALSE(dk::import_gltf(f.paths,limited));
}

TEST_CASE("unlit preview explicitly omits lighting inputs while strict import still rejects") {
    GltfFixture f; textured(f);
    f.json["materials"][0]["normalTexture"]={{"index",0}};
    f.json["materials"][0]["pbrMetallicRoughness"]["metallicRoughnessTexture"]={{"index",0}};
    const auto offset=f.binary.size();
    for (int i=0;i<3;++i) for (float value : {1.f,0.f,0.f,1.f}) GltfFixture::word(f.binary,std::bit_cast<std::uint32_t>(value));
    f.json["buffers"][0]["byteLength"]=f.binary.size();
    f.json["bufferViews"].push_back({{"buffer",0},{"byteOffset",offset},{"byteLength",48}});
    f.json["accessors"].push_back({{"bufferView",2},{"componentType",5126},{"count",3},{"type","VEC4"}});
    f.json["meshes"][0]["primitives"][0]["attributes"]["TANGENT"]=4;
    f.save(); REQUIRE_FALSE(dk::import_gltf(f.paths,{"assets/模型.gltf"}));
    dk::GltfImportRequest request{"assets/模型.gltf"}; request.profile=dk::GltfImportProfile::unlit_preview;
    auto preview=dk::import_gltf(f.paths,request); REQUIRE(preview);
    CHECK(preview->textures.size()==1); CHECK(preview->materials[0].alpha_mode==dk::AlphaMode::mask);
    CHECK(preview->diagnostics.size()==3); // Tangent, lighting textures, node placement.
    f.json["accessors"][4]["count"]=2; f.save(); CHECK_FALSE(dk::import_gltf(f.paths,request));
    f.json["accessors"][4]["count"]=3; f.json["materials"][0]["emissiveTexture"]={{"index",0}}; f.save();
    CHECK_FALSE(dk::import_gltf(f.paths,request));
}

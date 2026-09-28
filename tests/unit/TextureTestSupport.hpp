#pragma once
#include "GltfTestSupport.hpp"
inline dk::ByteBuffer image_fixture(std::string_view name)
{
    return dk::read_file_bytes(std::filesystem::path{DK_TEST_ASSET_DIR} / *dk::path_from_utf8(name)).value();
}
inline void textured(GltfFixture& f, std::string_view name = "rgba.png", bool embedded = false)
{
    using J = nlohmann::json;
    const auto bytes = image_fixture(name);
    f.json["textures"] = J::array({{{"source",0},{"sampler",0}}});
    f.json["samplers"] = J::array({{{"minFilter",9987},{"magFilter",9728},{"wrapS",33071},{"wrapT",33648}}});
    f.json["materials"][0]["pbrMetallicRoughness"]["baseColorTexture"] = {{"index",0}};
    if (embedded) {
        f.json["images"] = J::array({{{"bufferView",2},{"mimeType",name == "red.jpg" ? "image/jpeg" : "image/png"}}});
        f.json["bufferViews"].push_back({{"buffer",0},{"byteOffset",f.binary.size()},{"byteLength",bytes.size()}});
        f.binary.insert(f.binary.end(),bytes.begin(),bytes.end());
        f.json["buffers"][0]["byteLength"] = f.binary.size();
    } else {
        f.json["images"] = J::array({{{"uri","贴图.png"}}});
        std::filesystem::create_directories(f.files.root / "assets");
        REQUIRE(dk::write_file_bytes(f.files.root / *dk::path_from_utf8("assets/贴图.png"),bytes));
    }
}

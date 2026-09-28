#pragma once
#include "SceneTestFiles.hpp"
#include <dk/memory/MemorySystem.hpp>
#include <dk/assets/GltfImporter.hpp>
#include <nlohmann/json.hpp>
#include <bit>

struct ImportTestMemory {
    dk::memory::MemorySystem system = std::move(dk::memory::MemorySystem::create().value());
    dk::memory::ResourceHandle heap = system.create_heap({"import-tests", dk::memory::DomainCategory::assets}).value();
    dk::memory::ThreadContext context{system, heap};
    dk::memory::ExecutionScope scope{context, heap};
};
struct GltfFixture {
    ImportTestMemory memory;
    SceneTestFiles files;
    dk::ProjectPaths paths = dk::ProjectPaths::create(files.root).value();
    nlohmann::json json;
    std::vector<std::byte> binary;
    static void word(std::vector<std::byte>& bytes, std::uint32_t value)
    { for (unsigned i = 0; i < 4; ++i) { bytes.push_back(static_cast<std::byte>((value >> (8 * i)) & 255)); } }
    GltfFixture()
    {
        using J = nlohmann::json;
        for (const auto& vertex : {std::array{1.f,2.f,3.f,0.f,0.f,1.f,0.f,0.f}, std::array{-2.f,4.f,1.f,0.f,0.f,1.f,1.f,0.f}, std::array{0.f,0.f,-1.f,0.f,0.f,1.f,0.f,1.f}}) {
            for (const float value : vertex) { word(binary, std::bit_cast<std::uint32_t>(value)); }
        }
        for (const unsigned value : {0U, 1U, 2U}) { word(binary, value); }
        json = {{"asset", {{"version","2.0"}}}, {"buffers", J::array({{{"uri","数据.bin"},{"byteLength",binary.size()}}})},
            {"bufferViews",J::array({{{"buffer",0},{"byteOffset",0},{"byteLength",96},{"byteStride",32}}, {{"buffer",0},{"byteOffset",96},{"byteLength",12}}})},
            {"accessors",J::array({{{"bufferView",0},{"byteOffset",0},{"componentType",5126},{"count",3},{"type","VEC3"}},
                {{"bufferView",0},{"byteOffset",12},{"componentType",5126},{"count",3},{"type","VEC3"}},
                {{"bufferView",0},{"byteOffset",24},{"componentType",5126},{"count",3},{"type","VEC2"}},
                {{"bufferView",1},{"componentType",5125},{"count",3},{"type","SCALAR"}}})},
            {"meshes",J::array({{{"primitives",J::array({{{"attributes",{{"POSITION",0},{"NORMAL",1},{"TEXCOORD_0",2}}},{"indices",3},{"material",0}}})}}})},
            {"materials",J::array({{{"pbrMetallicRoughness",{{"baseColorFactor",J::array({0.2,0.4,0.6,0.8})},{"metallicFactor",0.3},{"roughnessFactor",0.7}}},
                {"emissiveFactor",J::array({0.1,0.2,0.3})},{"alphaMode","MASK"},{"alphaCutoff",0.25},{"doubleSided",true}}})},
            {"nodes",J::array({{{"mesh",0},{"translation",J::array({100,200,300})}}})}};
    }
    void save(std::string_view source = "assets/模型.gltf")
    {
        files.write(*dk::path_from_utf8(source), json.dump());
        const auto buffer = files.root / *dk::path_from_utf8("assets/数据.bin");
        REQUIRE(dk::write_file_bytes(buffer, binary));
    }
    void glb()
    {
        auto body = json; body["buffers"][0].erase("uri"); auto text = body.dump(); while (text.size() % 4) { text += ' '; }
        auto bytes = binary; while (bytes.size() % 4) { bytes.push_back(std::byte{}); }
        std::vector<std::byte> output; word(output, 0x46546c67); word(output, 2); word(output, static_cast<std::uint32_t>(28 + text.size() + bytes.size()));
        word(output, static_cast<std::uint32_t>(text.size())); word(output, 0x4e4f534a);
        const auto text_bytes = std::as_bytes(std::span{text.data(),text.size()}); output.insert(output.end(),text_bytes.begin(),text_bytes.end());
        word(output, static_cast<std::uint32_t>(bytes.size())); word(output, 0x004e4942); output.insert(output.end(),bytes.begin(),bytes.end());
        std::filesystem::create_directories(files.root / "assets"); REQUIRE(dk::write_file_bytes(files.root / *dk::path_from_utf8("assets/模型.glb"),output));
    }
};

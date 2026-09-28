#pragma once
#include <dk/assets/Catalog.hpp>
#include <dk/memory/MemorySystem.hpp>
#include "SceneTestFiles.hpp"

struct AssetTestMemory {
    dk::memory::MemorySystem system = std::move(dk::memory::MemorySystem::create().value());
    dk::memory::ResourceHandle heap = system.create_heap({"asset-tests", dk::memory::DomainCategory::assets}).value();
    dk::memory::ThreadContext context{system};
    dk::memory::ExecutionScope scope{context, heap};
};
inline dk::AssetMetadata test_meta()
{
    dk::AssetMetadata result;
    result.root_id = *dk::AssetId::parse("00112233-4455-4677-8899-aabbccddeeff");
    result.outputs.push_back({"mesh/0", result.root_id, dk::AssetKind::mesh});
    return result;
}
inline dk::RegistrationRequest first_registration(std::string_view source = "assets/模型.gltf")
{ return {source, {}, {}, dk::MissingMetaPolicy::create_or_adopt}; }
inline void write_test_meta(const SceneTestFiles& files, std::string_view source, const dk::AssetMetadata& meta)
{
    const auto encoded = dk::serialize_asset_meta(meta); REQUIRE(encoded);
    files.write(*dk::path_from_utf8(std::string{source} + ".meta"), *encoded);
}
inline void write_test_source(const SceneTestFiles& files, std::string_view source = "assets/模型.gltf")
{ files.write(*dk::path_from_utf8(source), "synthetic source; identity stage does not decode glTF"); }

#include <catch2/catch_test_macros.hpp>
#include "AssetTestSupport.hpp"
#include "../VoxelAssetSupport.hpp"
#include <dk/assets/MeshVoxelizer.hpp>
#include <spanstream>

namespace {
dk::CpuAsset plane() {
    dk::CpuAsset a; dk::MeshPrimitive p;
    p.positions={{-1,0,-1},{1,0,-1},{1,0,1},{-1,0,1}};
    p.texcoords={{0,0},{1,0},{1,1},{0,1}}; p.indices={0,1,2,0,2,3};
    a.mesh.primitives.push_back(std::move(p)); return a;
}
openvdb::Int32Grid::Ptr decode(const dk::assets::MeshVoxelization& v) {
    std::ispanstream bytes(std::span{reinterpret_cast<const char*>(v.vdb.data()),v.vdb.size()});
    openvdb::io::Stream stream(bytes); return openvdb::gridPtrCast<openvdb::Int32Grid>(stream.getGrids()->at(0));
}
}
TEST_CASE("mesh voxelizer preserves open surfaces and local coordinates") {
    AssetTestMemory memory; auto input=plane();
    const auto result=dk::assets::voxelize_mesh(input,{.resolution=16}); REQUIRE(result);
    const auto grid=decode(*result); REQUIRE(grid); REQUIRE(grid->background()==0); REQUIRE(grid->getGridClass()==openvdb::GRID_UNKNOWN);
    REQUIRE(grid->tree().getValue({8,0,8})>0); REQUIRE(grid->tree().getValue({8,-1,8})>0);
    REQUIRE(grid->tree().getValue({8,8,8})==0); // No spurious interior fill.
    REQUIRE(grid->indexToWorld(openvdb::Vec3d(0)).x()==-0.9375);
    const auto again=dk::assets::voxelize_mesh(input,{.resolution=16}); REQUIRE(again);
    REQUIRE(again->description_json==result->description_json);
    REQUIRE(decode(*again)->activeVoxelCount()==grid->activeVoxelCount());
    SceneTestFiles files; REQUIRE(dk::write_file_bytes(files.root/"grid.vdb",result->vdb)); files.write("solid.dkvoxel",result->description_json);
    const auto imported=dk::assets::import_voxel(dk::ProjectPaths::create(files.root).value(),{"solid.dkvoxel"}); REQUIRE(imported);
    REQUIRE(imported->voxel->grid->value({8,0,8})>0);
}
TEST_CASE("mesh voxelizer bakes colors and alpha mask while rejecting blend") {
    AssetTestMemory memory; auto input=plane();
    dk::MaterialData material; material.id=dk::AssetId::generate().value(); material.metallic=0; material.alpha_mode=dk::AlphaMode::mask;
    dk::TextureData texture; texture.id=dk::AssetId::generate().value(); texture.width=2; texture.height=1;
    texture.sampler.wrap_s=dk::TextureWrap::clamp; texture.sampler.wrap_t=dk::TextureWrap::clamp;
    for(unsigned b:{255,0,0,0,0,255,0,255}) texture.rgba8.push_back(std::byte(b));
    material.base_color_texture=texture.id; input.mesh.primitives[0].material=material.id;
    input.materials.push_back(material); input.textures.push_back(std::move(texture));
    const auto result=dk::assets::voxelize_mesh(input,{.resolution=16}); REQUIRE(result);
    const auto grid=decode(*result); REQUIRE(grid->tree().getValue({1,0,8})==0); REQUIRE(grid->tree().getValue({14,0,8})>0);
    const auto desc=nlohmann::json::parse(result->description_json);
    const auto color=desc["palette"][grid->tree().getValue({14,0,8})-1]["base_color"];
    REQUIRE(color[0]==0); REQUIRE(color[1]==1); REQUIRE(color[2]==0);
    input.materials[0].alpha_mode=dk::AlphaMode::blend; REQUIRE_FALSE(dk::assets::voxelize_mesh(input));
    REQUIRE(grid->tree().getValue({14,0,8})>0); // Prior owning result remains valid.
}
TEST_CASE("mesh voxelizer rejects bad parameters budgets cancellation and geometry") {
    AssetTestMemory memory; auto input=plane();
    REQUIRE_FALSE(dk::assets::voxelize_mesh(input,{.resolution=0}));
    REQUIRE_FALSE(dk::assets::voxelize_mesh(input,{.resolution=513}));
    REQUIRE_FALSE(dk::assets::voxelize_mesh(input,{.resolution=16,.maximum_candidates=1}));
    std::stop_source stop; stop.request_stop(); REQUIRE_FALSE(dk::assets::voxelize_mesh(input,{.stop=stop.get_token()}));
    input.mesh.primitives[0].indices[0]=999; REQUIRE_FALSE(dk::assets::voxelize_mesh(input));
}
TEST_CASE("mesh voxelizer keeps closed interiors empty and scale invariant") {
    AssetTestMemory memory; dk::CpuAsset asset; dk::MeshPrimitive p;
    p.positions={{-1,-1,-1},{1,-1,-1},{1,1,-1},{-1,1,-1},{-1,-1,1},{1,-1,1},{1,1,1},{-1,1,1}};
    p.indices={0,2,1,0,3,2,4,5,6,4,6,7,0,1,5,0,5,4,3,7,6,3,6,2,0,4,7,0,7,3,1,2,6,1,6,5};
    asset.mesh.primitives.push_back(std::move(p));
    const auto result=dk::assets::voxelize_mesh(asset,{.resolution=16}); REQUIRE(result); const auto original=decode(*result);
    REQUIRE(original->tree().getValue({8,8,8})==0); REQUIRE(original->tree().getValue({0,8,8})>0);
    for(auto& v:asset.mesh.primitives[0].positions) v=v*3+dk::Vec3f{4,-2,7};
    const auto moved=dk::assets::voxelize_mesh(asset,{.resolution=16}); REQUIRE(moved); const auto transformed=decode(*moved);
    REQUIRE(original->activeVoxelCount()==transformed->activeVoxelCount());
    for(int z=-1;z<=16;++z) for(int y=-1;y<=16;++y) for(int x=-1;x<=16;++x)
        REQUIRE(original->tree().getValue({x,y,z})==transformed->tree().getValue({x,y,z}));
}

#include "SceneTestFiles.hpp"
#include "../VoxelAssetSupport.hpp"
#include <dk/assets/AssetCache.hpp>
#include <dk/render/DiskScene.hpp>
#include <dk/scene/SceneIO.hpp>
#include <dk/memory/MemorySystem.hpp>
using namespace dk;
using namespace dk::render;
namespace {
struct VoxelDiskFixture {
    memory::MemorySystem system=std::move(memory::MemorySystem::create().value());
    memory::ResourceHandle heap=system.create_heap({"voxel-disk",memory::DomainCategory::render}).value();
    memory::ThreadContext context{system,heap};
    memory::ExecutionScope scope{context,heap};
    SceneTestFiles files;
    AssetId id=AssetId::generate().value();
    Project project=Project::create(files.root,{"Voxel disk","scene.json",{{id,AssetKind::voxel,"assets/solid.dkvoxel"}}}).value();
    std::unique_ptr<SceneDocument> scene=SceneDocument::create().value();
    VoxelDiskFixture() {
        voxel_fixture::write(files.root);
        for(int i=0;i<2;++i) {
            const auto e=EntityId::generate().value(); REQUIRE(scene->create_entity(e)); REQUIRE(scene->set_asset_references(e,{{id,AssetKind::voxel}}));
        }
    }
    Result<DiskScene> load(DiskSceneOptions options={}) { return DiskScene::load_snapshot(heap,project,scene->snapshot().value(),options); }
};
}
TEST_CASE("voxel disk candidate is read only deduplicated bounded and survives source replacement") {
    VoxelDiskFixture f; auto old=f.load(); INFO((old ? "loaded" : old.error().message)); REQUIRE(old);
    REQUIRE(old->assets().size()==1); CHECK(old->has_voxels()); CHECK(old->cpu_bytes()>0);
    CHECK(root_asset_id(*old->assets()[0])==f.id); CHECK(old->assets()[0]->voxel->grid->value({-8,0,0})==1);
    CHECK_FALSE(std::filesystem::exists(f.files.root/"assets/solid.dkvoxel.meta")); CHECK_FALSE(std::filesystem::exists(f.files.root/".cache"));
    DiskSceneOptions small; small.cpu_bytes=1; CHECK_FALSE(f.load(small));
    std::stop_source stop; stop.request_stop(); small={}; small.stop=stop.get_token(); CHECK_FALSE(f.load(small));
    f.files.write("assets/solid.dkvoxel","invalid"); CHECK_FALSE(f.load());
    CHECK(old->assets()[0]->voxel->grid->value({-8,0,0})==1);
    auto hit=old->assets()[0]->voxel->grid->nearest({{-.5,.5,-1},{0,0,1},0,10}); REQUIRE(hit); REQUIRE(hit->hit); CHECK(hit->hit->distance==1);
    f.heap.begin_close(); CHECK_FALSE(f.load()); CHECK(old->assets()[0]->voxel->grid->value({-8,0,0})==1);
}
TEST_CASE("voxel disk artifact verifies identity and explicit inspection leaves scene unchanged") {
    VoxelDiskFixture f;
    auto built=compile_cached_asset(f.project.paths(),{"assets/solid.dkvoxel"}); REQUIRE(built);
    CHECK_FALSE(f.load()); // Newly generated metadata cannot silently replace the Project identity.
    auto description=f.project.description(); description.assets[0].id=root_asset_id(built->artifact.data);
    for(const auto& e:f.scene->snapshot()->entities()) REQUIRE(f.scene->set_asset_references(e.id,{{description.assets[0].id,AssetKind::voxel}}));
    f.project=Project::create(f.files.root,description).value(); REQUIRE(f.load());
    description.assets[0].path=std::string{built->directory}+"/manifest.json";
    f.project=Project::create(f.files.root,description).value(); auto artifact=f.load(); REQUIRE(artifact);
    CHECK(artifact->assets()[0]->voxel->grid->value({-8,0,0})==1);
    REQUIRE(std::filesystem::remove(f.files.root/"assets/grid.vdb")); REQUIRE(f.load()); // Artifact is independent of the source.
    EnvironmentDescription env; env.enabled=true; REQUIRE(f.scene->set_environment(env));
    const auto revision=f.scene->snapshot()->revision(); DiskSceneOptions options; options.unlit_inspection=true;
    auto inspection=f.load(options); REQUIRE(inspection); CHECK_FALSE(inspection->scene().environment().enabled);
    CHECK(f.scene->snapshot()->environment().enabled); CHECK(f.scene->snapshot()->revision()==revision);
}

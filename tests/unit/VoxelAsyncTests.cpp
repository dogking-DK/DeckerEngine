#include "AssetTestSupport.hpp"
#include "../VoxelAssetSupport.hpp"
#include <dk/assets/AsyncAssets.hpp>
namespace {
using namespace dk;
JobSnapshot finish(AsyncAssets& assets,JobQueue& queue,JobId id) {
    const auto deadline=JobQueue::Clock::now()+std::chrono::seconds(15);
    for(;;) {
        const auto sequence=queue.change_sequence(); assets.pump(); auto state=queue.query(id); REQUIRE(state);
        if(terminal(state->state)) return *state;
        REQUIRE(JobQueue::Clock::now()<deadline); queue.wait_change(sequence,deadline);
    }
}
}
TEST_CASE("voxel asynchronous Ready replacement unload and owner exit preserve snapshots", "[voxel-assets]") {
    const auto retained=[] {
        AssetTestMemory memory; SceneTestFiles files; voxel_fixture::write(files.root);
        const auto paths=ProjectPaths::create(files.root).value(); auto cached=compile_cached_asset(paths,{"assets/solid.dkvoxel"}); REQUIRE(cached);
        const auto id=root_asset_id(cached->artifact.data);
        JobQueue queue(*memory.system.create_heap({"voxel-jobs",memory::DomainCategory::jobs})); AsyncAssets assets(paths,queue);
        auto loading=assets.load(id,"assets/solid.dkvoxel"); REQUIRE(loading); REQUIRE(loading->job);
        const auto loaded=finish(assets,queue,*loading->job); INFO(static_cast<int>(loaded.state)); REQUIRE(loaded.state==JobState::succeeded);
        const auto old=assets.status(id,"assets/solid.dkvoxel"); REQUIRE(old.data); CHECK(root_asset_id(old.data->artifact.data)==id);
        auto j=voxel_fixture::description(); j["palette"][0]["roughness"]=.25; voxel_fixture::write_text(files.root/"assets/solid.dkvoxel",j);
        auto job=assets.import("assets/solid.dkvoxel"); REQUIRE(job); CHECK(finish(assets,queue,*job).state==JobState::succeeded);
        const auto next=assets.status(id,"assets/solid.dkvoxel"); REQUIRE(next.data); CHECK(next.generation>old.generation);
        CHECK(next.data->artifact.data.voxel->palette[0].roughness==.25f); CHECK(old.data->artifact.data.voxel->palette[0].roughness==1);
        auto unloaded=assets.unload(id,"assets/solid.dkvoxel"); REQUIRE(unloaded); CHECK(unloaded->state==AssetState::unloaded); CHECK_FALSE(unloaded->data);
        CHECK(next.data->artifact.data.voxel->grid->value({-8,0,0})==1);
        queue.close(); return old.data;
    }();
    CHECK(retained->artifact.data.voxel->grid->value({-8,0,0})==1); CHECK(retained->artifact.data.voxel->palette[0].roughness==1);
}

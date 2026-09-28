#include "GltfTestSupport.hpp"
#include "AssetCacheInternal.hpp"
#include <dk/assets/AsyncAssets.hpp>
#include <atomic>
#include <latch>

namespace {
using namespace dk;
constexpr std::string_view source = "assets/模型.gltf";
JobSnapshot finish(AsyncAssets& assets, JobQueue& queue, JobId id)
{
    const auto deadline = JobQueue::Clock::now() + std::chrono::seconds(10);
    for (;;) {
        const auto sequence = queue.change_sequence(); assets.pump();
        auto state = queue.query(id); REQUIRE(state);
        if (terminal(state->state)) return *state;
        REQUIRE(JobQueue::Clock::now() < deadline); queue.wait_change(sequence, deadline);
    }
}
auto meta_path(const GltfFixture& f) { return f.files.root / *path_from_utf8(std::string{source} + ".meta"); }
auto current_path(const GltfFixture& f, AssetId id) { return f.files.root / ".decker/cache/assets/v1/current" / (id.to_string()+".json"); }
struct Gate {
    std::latch prepared{1}, release{1}; std::atomic<int> calls{0};
    AsyncAssets::Prepare callback() { return [this](const ProjectPaths& paths, const AssetCacheRequest& r, std::stop_token stop) -> Result<PreparedCachedAsset> {
        auto value = prepare_cached_asset(paths, r, stop);
        if (++calls == 1) { prepared.count_down(); release.wait(); }
        return value;
    }; }
};
}
TEST_CASE("prepared cache defers metadata and current and rejects changed input")
{
    GltfFixture f; f.save();
    auto prepared = dk::prepare_cached_asset(f.paths, {source}); REQUIRE(prepared);
    auto id = prepared->value().artifact.data.mesh.id;
    CHECK_FALSE(std::filesystem::exists(meta_path(f))); CHECK_FALSE(std::filesystem::exists(current_path(f,id)));
    f.binary[0] ^= std::byte{1}; REQUIRE(dk::write_file_bytes(f.files.root / *dk::path_from_utf8("assets/数据.bin"), f.binary));
    CHECK_FALSE(prepared->publish()); CHECK_FALSE(std::filesystem::exists(meta_path(f)));
    auto retry = dk::prepare_cached_asset(f.paths, {source}); REQUIRE(retry); REQUIRE(retry->publish()); CHECK_FALSE(retry->publish());
}
TEST_CASE("async cancelled prepared work never publishes metadata or current")
{
    GltfFixture f; f.save(); Gate gate;
    dk::JobQueue queue(*f.memory.system.create_heap({"jobs", dk::memory::DomainCategory::jobs}));
    dk::AsyncAssets assets(f.paths, queue, gate.callback());
    auto id = assets.import(source); REQUIRE(id); gate.prepared.wait();
    CHECK_FALSE(std::filesystem::exists(meta_path(f)));
    const auto cancelled = queue.request_cancel(*id); gate.release.count_down(); REQUIRE(cancelled); CHECK(cancelled->accepted);
    CHECK(finish(assets,queue,*id).state == dk::JobState::cancelled);
    CHECK_FALSE(std::filesystem::exists(meta_path(f)));
    CHECK(std::filesystem::is_empty(f.files.root/".decker/cache/assets/v1/current"));
}
TEST_CASE("async load shares loading and ready handles survive unload and memory owner exit")
{
    auto retained = [] {
        GltfFixture f; f.save(); auto cached = dk::compile_cached_asset(f.paths,{source}); REQUIRE(cached);
        auto root = cached->artifact.data.mesh.id;
        dk::JobQueue queue(*f.memory.system.create_heap({"jobs",dk::memory::DomainCategory::jobs}));
        dk::AsyncAssets assets(f.paths,queue); auto load = assets.load(root,source); REQUIRE(load); REQUIRE(load->job);
        CHECK(assets.load(root,source)->job == load->job);
        CHECK(finish(assets,queue,*load->job).state == dk::JobState::succeeded);
        auto ready = assets.status(root,source); REQUIRE(ready.data); CHECK(ready.state == dk::AssetState::ready);
        CHECK(ready.ready_generation == ready.generation); CHECK(ready.data->cache_hit);
        CHECK(assets.load(root,source)->data == ready.data);
        auto unloaded = assets.unload(root,source); REQUIRE(unloaded); CHECK(unloaded->state == dk::AssetState::unloaded);
        CHECK_FALSE(unloaded->data); CHECK(ready.data->artifact.data.mesh.primitives[0].positions.size() == 3);
        queue.close(); return ready.data;
    }();
    CHECK(retained->artifact.data.mesh.primitives[0].positions[0] == dk::Vec3f{1,2,3});
}
TEST_CASE("async old generation and old session cannot overwrite replacement")
{
    GltfFixture f; f.save(); auto cached = dk::compile_cached_asset(f.paths,{source}); REQUIRE(cached);
    const auto root = cached->artifact.data.mesh.id;
    const auto before = dk::asset_detail::read_cache_text(current_path(f,root),16384);
    Gate gate; dk::JobQueue queue(*f.memory.system.create_heap({"jobs",dk::memory::DomainCategory::jobs}));
    dk::AsyncAssets assets(f.paths,queue,gate.callback()); auto old = assets.import(source,2); REQUIRE(old);
    gate.prepared.wait();
    const auto unloaded = assets.unload(root,source);
    const auto reset = assets.reset_session();
    const auto next = assets.import(source,3);
    gate.release.count_down(); REQUIRE(unloaded); REQUIRE(reset); REQUIRE(next);
    CHECK(dk::asset_detail::read_cache_text(current_path(f,root),16384) == before);
    CHECK(finish(assets,queue,*old).state == dk::JobState::cancelled);
    CHECK(finish(assets,queue,*next).state == dk::JobState::succeeded);
    const auto ready = assets.status(root,source); REQUIRE(ready.data); CHECK(ready.data->artifact.data.unit_scale == 3);
    CHECK(ready.data->artifact.data.mesh.id == root);
}
TEST_CASE("async failure is retryable and rejected submissions preserve ready state")
{
    GltfFixture f; f.save(); auto cached = dk::compile_cached_asset(f.paths,{source}); REQUIRE(cached); const auto root = cached->artifact.data.mesh.id;
    dk::JobQueue queue(*f.memory.system.create_heap({"jobs",dk::memory::DomainCategory::jobs}));
    dk::AsyncAssets assets(f.paths,queue);
    std::filesystem::remove(f.files.root / *dk::path_from_utf8("assets/数据.bin"));
    auto load = assets.load(root,source); REQUIRE(load); REQUIRE(load->job);
    CHECK(finish(assets,queue,*load->job).state == dk::JobState::failed);
    CHECK(assets.status(root,source).state == dk::AssetState::failed);
    f.save(); auto retry = assets.load(root,source); REQUIRE(retry); REQUIRE(retry->job);
    CHECK(finish(assets,queue,*retry->job).state == dk::JobState::succeeded);
    auto ready = assets.status(root,source); CHECK_FALSE(assets.import(source,-1));
    CHECK(assets.status(root,source).data == ready.data); CHECK_FALSE(assets.import("../escape.gltf"));
}

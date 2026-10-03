#include <dk/render/RenderScene.hpp>
#include <dk/scene/SceneDocument.hpp>
#include <dk/memory/MemorySystem.hpp>
#include <dk/math/Math.hpp>
#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <limits>

using namespace dk;
using namespace dk::render;
namespace {
struct Fixture {
    memory::MemorySystem system = std::move(memory::MemorySystem::create().value());
    memory::ResourceHandle heap = system.create_heap({"render-data", memory::DomainCategory::render}).value();
};
}
TEST_CASE("render extraction preserves hierarchy shear references and immutable revision") {
    Fixture f;
    auto scene = std::move(SceneDocument::create().value());
    auto parent = scene->create_entity().value(); auto child = scene->create_entity().value();
    Trsd a; a.scale = {2,3,-1}; a.translation = {1,2,3};
    Trsd b; b.rotation = rotation_from_axis_angle(Vec3d{0,0,1}, radians(45.0)).value();
    REQUIRE(scene->set_local_transform(parent,a)); REQUIRE(scene->set_local_transform(child,b));
    REQUIRE(scene->set_parent(child,parent));
    const std::vector refs{AssetReference{AssetId::generate().value(),AssetKind::texture},
        AssetReference{AssetId::generate().value(),AssetKind::mesh}};
    REQUIRE(scene->set_asset_references(child,refs));
    const auto revision = scene->revision(); const bool dirty = scene->dirty();
    auto extracted = RenderScene::extract(f.heap,scene->snapshot().value()).value();
    CHECK(scene->revision() == revision); CHECK(scene->dirty() == dirty);
    REQUIRE(extracted.entities().size() == 2);
    CHECK(extracted.entities()[0].id < extracted.entities()[1].id);
    const auto index = extracted.entities()[0].id == child ? 0u : 1u;
    CHECK(extracted.entities()[index].world.matrix() == scene->world_transform(child)->matrix());
    CHECK(std::ranges::equal(extracted.assets(index).value(),refs));
    CHECK_FALSE(extracted.assets(2));
    a.scale = {0,0,0}; REQUIRE(scene->set_local_transform(parent,a));
    auto next = RenderScene::extract(f.heap,scene->snapshot().value()).value();
    CHECK(extracted.revision() == revision); CHECK(next.revision() > revision);
    CHECK(next.entities()[index].world.matrix() != extracted.entities()[index].world.matrix());
    auto copy = extracted; extracted = {}; scene.reset(); f.heap.begin_close();
    CHECK(copy.entities().size() == 2); CHECK(std::ranges::equal(copy.assets(index).value(),refs));
    CHECK_FALSE(RenderScene::extract(f.heap,SceneDocument::create().value()->snapshot().value()));
    copy = {}; next = {}; CHECK(f.heap.snapshot().live_allocations == 0);
}
TEST_CASE("render view owns its scene and validates camera projection and extent") {
    Fixture f; auto source = std::move(SceneDocument::create().value());
    auto scene = RenderScene::extract(f.heap,source->snapshot().value()).value();
    CHECK(scene.entities().empty()); CHECK_FALSE(scene.assets(0));
    ViewDescription desc{640,480,7}; Trsd camera; camera.translation = {2,3,4};
    desc.camera_world = Transformd::from_trs(camera).value(); desc.projection(0,0) = 2;
    auto view = RenderView::create(scene,desc).value();
    CHECK(view.description().frame == 7); CHECK(view.world_to_clip()(0,3) == -4);
    CHECK_FALSE(RenderView::create({},desc));
    desc.width = 0; CHECK_FALSE(RenderView::create(scene,desc)); desc.width = 1;
    desc.projection.setZero(); CHECK_FALSE(RenderView::create(scene,desc));
    desc.projection.setIdentity(); desc.projection(0,0) = std::numeric_limits<double>::infinity();
    CHECK_FALSE(RenderView::create(scene,desc)); desc.projection.setIdentity();
    camera.scale.x() = 0; desc.camera_world = Transformd::from_trs(camera).value();
    CHECK_FALSE(RenderView::create(scene,desc));
    scene = {}; source.reset(); f.heap.begin_close(); CHECK(view.scene().id() != SceneId{});
}
TEST_CASE("render extraction budget failure unwinds candidate and preserves prior extraction") {
    Fixture f; const auto heap = f.system.create_heap({"budget",memory::DomainCategory::render,16384}).value();
    auto source = std::move(SceneDocument::create().value());
    for (int i = 0; i < 8; ++i) REQUIRE(source->create_entity());
    auto snapshot = source->snapshot().value();
    auto prior = RenderScene::extract(heap,snapshot).value();
    struct Block { void* pointer; std::size_t size; }; std::vector<Block> blocks;
    while (auto b = heap.try_allocate(128)) blocks.push_back({*b,128});
    while (auto b = heap.try_allocate(1)) blocks.push_back({*b,1});
    bool failed = false, partial = false, success = false;
    do {
        const auto before = heap.snapshot();
        {
            auto result = RenderScene::extract(heap,snapshot);
            if (!result) { failed = true; partial |= heap.snapshot().allocation_count > before.allocation_count + 2; }
            else { success = true; CHECK(result->entities().size() == 8); }
        }
        CHECK(heap.snapshot().live_allocations == before.live_allocations);
        CHECK(heap.snapshot().backing_requested_bytes == before.backing_requested_bytes);
        CHECK(prior.revision() == snapshot.revision());
        if (blocks.empty()) break;
        auto block = blocks.back(); blocks.pop_back(); heap.deallocate(block.pointer,block.size);
    } while (true);
    CHECK(failed); CHECK(partial); CHECK(success);
}

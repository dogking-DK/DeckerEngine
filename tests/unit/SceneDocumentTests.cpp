#include <dk/scene/SceneDocument.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <type_traits>

static_assert(!std::is_copy_constructible_v<dk::SceneDocument>);
static_assert(!std::is_move_constructible_v<dk::SceneDocument>);

TEST_CASE("scene owns an initially unsaved empty ECS world", "[scene]")
{
    auto result = dk::SceneDocument::create();
    REQUIRE(result.has_value());
    auto& scene = **result;
    REQUIRE_FALSE(scene.id().is_nil());
    REQUIRE(scene.revision() == 0);
    REQUIRE(scene.dirty());
    REQUIRE(scene.entity_count() == 0);
    REQUIRE(scene.entity_ids().empty());
    REQUIRE(scene.validate().has_value());
    auto owner = std::move(*result);
    REQUIRE(owner->validate().has_value());
}

TEST_CASE("scene accepts a persistent scene ID and rejects nil", "[scene]")
{
    const auto id = dk::SceneId::parse("00112233-4455-4677-8899-aabbccddeeff");
    REQUIRE(id.has_value());
    const auto scene = dk::SceneDocument::create(*id);
    REQUIRE(scene.has_value());
    REQUIRE((*scene)->id() == *id);
    const auto invalid = dk::SceneDocument::create(dk::SceneId{});
    REQUIRE_FALSE(invalid.has_value());
    REQUIRE(invalid.error().code == dk::ErrorCode::invalid_argument);
}

TEST_CASE("scene creates persistent entities and removes identity mappings", "[scene]")
{
    auto scene = dk::SceneDocument::create();
    REQUIRE(scene.has_value());
    const auto first = (*scene)->create_entity();
    const auto second = (*scene)->create_entity();
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    REQUIRE_FALSE(first->is_nil());
    REQUIRE(*first != *second);
    REQUIRE((*scene)->entity_count() == 2);
    REQUIRE((*scene)->revision() == 2);
    REQUIRE((*scene)->validate().has_value());
    REQUIRE((*scene)->destroy_entity(*first).has_value());
    REQUIRE_FALSE((*scene)->contains(*first));
    REQUIRE((*scene)->contains(*second));
    REQUIRE((*scene)->revision() == 3);
    const auto third = (*scene)->create_entity();
    REQUIRE(third.has_value());
    REQUIRE(*third != *first);
    REQUIRE_FALSE((*scene)->contains(*first));
    REQUIRE((*scene)->validate().has_value());
}

TEST_CASE("scene rejected edits preserve entities and revision", "[scene]")
{
    auto scene = dk::SceneDocument::create();
    REQUIRE(scene.has_value());
    const auto id = (*scene)->create_entity();
    REQUIRE(id.has_value());
    const auto before = (*scene)->entity_ids();
    const auto revision = (*scene)->revision();
    const auto duplicate = (*scene)->create_entity(*id);
    REQUIRE_FALSE(duplicate.has_value());
    REQUIRE(duplicate.error().code == dk::ErrorCode::invalid_argument);
    const auto nil = (*scene)->create_entity(dk::EntityId{});
    REQUIRE_FALSE(nil.has_value());
    REQUIRE(nil.error().code == dk::ErrorCode::invalid_argument);
    const auto absent = (*scene)->destroy_entity(dk::EntityId{});
    REQUIRE_FALSE(absent.has_value());
    REQUIRE(absent.error().code == dk::ErrorCode::not_found);
    REQUIRE((*scene)->entity_ids() == before);
    REQUIRE((*scene)->revision() == revision);
    REQUIRE((*scene)->validate().has_value());
    REQUIRE((*scene)->destroy_entity(*id).has_value());
    const auto removed_revision = (*scene)->revision();
    REQUIRE_FALSE((*scene)->destroy_entity(*id).has_value());
    REQUIRE((*scene)->revision() == removed_revision);
    REQUIRE((*scene)->entity_count() == 0);
}

TEST_CASE("scene entity enumeration is sorted and independent of input order", "[scene]")
{
    auto scene = dk::SceneDocument::create();
    REQUIRE(scene.has_value());
    std::vector<dk::EntityId> ids;
    for (int index = 0; index < 128; ++index) {
        const auto id = (*scene)->create_entity();
        REQUIRE(id.has_value());
        ids.push_back(*id);
    }
    std::sort(ids.begin(), ids.end());
    REQUIRE((*scene)->entity_ids() == ids);
    for (std::size_t index = 0; index < ids.size(); index += 2) {
        REQUIRE((*scene)->destroy_entity(ids[index]).has_value());
    }
    REQUIRE((*scene)->entity_count() == 64);
    REQUIRE((*scene)->validate().has_value());
}

TEST_CASE("scene worlds isolate matching imported IDs and independent lifetimes", "[scene]")
{
    auto first = dk::SceneDocument::create();
    auto second = dk::SceneDocument::create();
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    const auto id = (*first)->create_entity();
    REQUIRE(id.has_value());
    REQUIRE((*second)->create_entity(*id).has_value());
    first->reset();
    REQUIRE((*second)->contains(*id));
    REQUIRE((*second)->validate().has_value());
    REQUIRE((*second)->destroy_entity(*id).has_value());
    for (int index = 0; index < 16; ++index) {
        auto temporary = dk::SceneDocument::create();
        REQUIRE(temporary.has_value());
        REQUIRE((*temporary)->create_entity(*id).has_value());
        REQUIRE((*temporary)->validate().has_value());
    }
    REQUIRE((*second)->entity_count() == 0);
    REQUIRE((*second)->validate().has_value());
}

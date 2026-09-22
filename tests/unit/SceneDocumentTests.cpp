#include <dk/scene/SceneDocument.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <dk/math/Math.hpp>
#include <limits>
#include <type_traits>

TEST_CASE("scene component descriptors expose stable versions and property types", "[scene]")
{
    const auto descriptors = dk::scene_component_descriptors();
    REQUIRE(descriptors.size() == 4);
    REQUIRE(descriptors[0].name == "dk.Identity");
    REQUIRE(descriptors[0].properties[0].read_only);
    REQUIRE(descriptors[2].name == "dk.Transform");
    REQUIRE(descriptors[2].properties.size() == 3);
    REQUIRE(descriptors[2].properties[1].type == dk::PropertyType::quaternion);
    for (const auto& descriptor : descriptors) { REQUIRE(descriptor.version == 1); }
}

TEST_CASE("scene names are owned UTF-8 values and invalid or unchanged edits preserve revision", "[scene]")
{
    auto scene = *dk::SceneDocument::create();
    const auto id = *scene->create_entity();
    REQUIRE(scene->entity(id)->name.empty());
    REQUIRE(scene->set_name(id, "父节点/😀").has_value());
    const auto revision = scene->revision();
    auto copy = *scene->entity(id);
    copy.name = "copy";
    REQUIRE(scene->entity(id)->name == "父节点/😀");
    REQUIRE(scene->set_name(id, "父节点/😀").has_value());
    REQUIRE(scene->revision() == revision);
    for (const auto& bad : {std::string(1025, 'a'), std::string("x\0y", 3),
         std::string("\xc0\x80", 2), std::string("\xed\xa0\x80", 3), std::string("\xf4\x90\x80\x80", 4)}) {
        REQUIRE_FALSE(scene->set_name(id, bad).has_value());
        REQUIRE(scene->revision() == revision);
        REQUIRE(scene->entity(id)->name == "父节点/😀");
    }
    REQUIRE_FALSE(scene->entity({}).has_value());
    REQUIRE_FALSE(scene->set_name({}, "absent").has_value());
    REQUIRE_FALSE(scene->world_transform({}).has_value());
}

TEST_CASE("scene hierarchy propagates full affine worlds and preserves local TRS when reparenting", "[scene]")
{
    auto scene = *dk::SceneDocument::create();
    const auto parent = *scene->create_entity();
    const auto child = *scene->create_entity();
    const auto leaf = *scene->create_entity();
    dk::Trsd a;
    a.translation = {10, 0, 0};
    a.scale = {2, 3, -1};
    a.rotation = *dk::rotation_from_axis_angle(dk::Vec3d{0, 0, 1}, dk::radians(90.0));
    dk::Trsd b;
    b.translation = {1, 2, 3};
    b.rotation = *dk::rotation_from_axis_angle(dk::Vec3d{0, 0, 1}, dk::radians(45.0));
    REQUIRE(scene->set_local_transform(parent, a).has_value());
    REQUIRE(scene->set_local_transform(child, b).has_value());
    REQUIRE(scene->set_parent(child, parent).has_value());
    REQUIRE(scene->set_parent(leaf, child).has_value());
    auto expected = dk::Transformd::from_trs(a)->compose(*dk::Transformd::from_trs(b));
    REQUIRE(scene->world_transform(leaf)->matrix().isApprox(expected->matrix(), 1e-12));
    REQUIRE(scene->entity(child)->local.translation == b.translation);
    a.translation = {20, 1, 2};
    REQUIRE(scene->set_local_transform(parent, a).has_value());
    expected = dk::Transformd::from_trs(a)->compose(*dk::Transformd::from_trs(b));
    REQUIRE(scene->world_transform(leaf)->matrix().isApprox(expected->matrix(), 1e-12));
    REQUIRE(scene->set_parent(child, std::nullopt).has_value());
    REQUIRE(scene->world_transform(leaf)->matrix().isApprox(dk::Transformd::from_trs(b)->matrix(), 1e-12));
    REQUIRE(scene->destroy_entity(parent).has_value());
    REQUIRE(scene->validate().has_value());
}

TEST_CASE("scene rejects hierarchy cycles missing parents and nonleaf deletion without mutation", "[scene]")
{
    auto scene = *dk::SceneDocument::create();
    const auto a = *scene->create_entity();
    const auto b = *scene->create_entity();
    const auto c = *scene->create_entity();
    REQUIRE(scene->set_parent(b, a).has_value());
    REQUIRE(scene->set_parent(c, b).has_value());
    const auto revision = scene->revision();
    REQUIRE_FALSE(scene->set_parent(a, c).has_value());
    REQUIRE_FALSE(scene->set_parent(a, a).has_value());
    REQUIRE_FALSE(scene->set_parent(a, dk::EntityId{}).has_value());
    REQUIRE_FALSE(scene->set_parent(a, *dk::EntityId::generate()).has_value());
    REQUIRE_FALSE(scene->destroy_entity(a).has_value());
    REQUIRE_FALSE(scene->destroy_entity(b).has_value());
    REQUIRE(scene->set_parent(b, a).has_value());
    REQUIRE(scene->revision() == revision);
    REQUIRE_FALSE(scene->entity(a)->parent.has_value());
    REQUIRE(scene->entity(b)->parent == a);
    REQUIRE(scene->validate().has_value());
    REQUIRE(scene->destroy_entity(c).has_value());
    REQUIRE(scene->destroy_entity(b).has_value());
    REQUIRE(scene->destroy_entity(a).has_value());
}

TEST_CASE("scene transform validation normalizes quaternions and rolls back descendant overflow", "[scene]")
{
    auto scene = *dk::SceneDocument::create();
    const auto parent = *scene->create_entity();
    const auto child = *scene->create_entity();
    dk::Trsd local;
    local.rotation = dk::Quatd{2, 0, 0, 0};
    const auto initial_revision = scene->revision();
    REQUIRE(scene->set_local_transform(child, local).has_value());
    REQUIRE(scene->revision() == initial_revision);
    local.scale = {2, 1, 1};
    REQUIRE(scene->set_local_transform(child, local).has_value());
    REQUIRE(scene->set_parent(child, parent).has_value());
    const auto before = *scene->world_transform(child);
    const auto revision = scene->revision();
    dk::Trsd bad;
    bad.scale.x() = std::numeric_limits<double>::max();
    REQUIRE_FALSE(scene->set_local_transform(parent, bad).has_value());
    bad.translation.x() = std::numeric_limits<double>::infinity();
    REQUIRE_FALSE(scene->set_local_transform(child, bad).has_value());
    bad = {};
    bad.rotation.coeffs().setZero();
    REQUIRE_FALSE(scene->set_local_transform(child, bad).has_value());
    REQUIRE(scene->revision() == revision);
    REQUIRE(scene->world_transform(child)->matrix() == before.matrix());
    REQUIRE(scene->entity(parent)->local.scale == dk::Vec3d::Ones());
    local.scale = {0, -2, 3};
    REQUIRE(scene->set_local_transform(child, local).has_value());
    REQUIRE_FALSE(scene->world_transform(child)->inverse().has_value());
    REQUIRE(scene->validate().has_value());
}

TEST_CASE("scene hierarchy traversal handles deep chains without recursive calls", "[scene]")
{
    auto scene = *dk::SceneDocument::create();
    std::optional<dk::EntityId> previous;
    dk::Trsd local;
    local.translation.x() = 1;
    for (int index = 0; index < 160; ++index) {
        const auto id = *scene->create_entity();
        REQUIRE(scene->set_local_transform(id, local).has_value());
        REQUIRE(scene->set_parent(id, previous).has_value());
        previous = id;
    }
    REQUIRE(scene->world_transform(*previous)->matrix()(0, 3) == 160);
    REQUIRE(scene->validate().has_value());
}


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

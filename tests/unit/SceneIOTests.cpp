#include <dk/scene/SceneIO.hpp>
#include <dk/math/Math.hpp>
#include "SceneTestFiles.hpp"
#include <nlohmann/json.hpp>
#include <limits>
#include <stdexcept>

TEST_CASE("scene rotation values remain stable across repeated serialization and no-op writes", "[scene][persistence]")
{
    SceneTestFiles files;
    const auto project = dk::Project::create(files.root, {"precision", "scene.json", {}});
    REQUIRE(project.has_value());
    auto doc = *dk::SceneDocument::create();
    const auto id = *doc->create_entity();
    for (int index = 0; index < 32; ++index) {
        dk::Trsd local;
        local.rotation = dk::Quatd{0.13 + 0.017 * index, 0.3, -0.7, 0.11};
        REQUIRE(doc->set_local_transform(id, local).has_value());
        const auto revision = doc->revision();
        const auto stored = doc->entity(id)->local;
        REQUIRE(doc->set_local_transform(id, stored).has_value());
        REQUIRE(doc->revision() == revision);
        const auto text = dk::serialize_scene(*doc->snapshot(), *project);
        REQUIRE(text.has_value());
        auto parsed = dk::parse_scene(*text, *project);
        REQUIRE(parsed.has_value());
        REQUIRE((*parsed)->entity(id)->local.rotation.coeffs() == stored.rotation.coeffs());
        REQUIRE(*dk::serialize_scene(*(*parsed)->snapshot(), *project) == *text);
    }
}

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace {
using Json = nlohmann::json;
struct Fixture {
    SceneTestFiles files;
    dk::AssetId asset = *dk::AssetId::generate();
    dk::Project project = *dk::Project::create(files.root,
        {"测试工程", "scene.json", {{asset, dk::AssetKind::mesh, "mesh.bin"}}});
    std::unique_ptr<dk::SceneDocument> doc = *dk::SceneDocument::create();
    dk::EntityId parent = *dk::EntityId::parse("ffffffff-ffff-4fff-8fff-ffffffffffff");
    dk::EntityId child = *dk::EntityId::parse("00000001-0000-4000-8000-000000000001");
    Fixture()
    {
        files.write("mesh.bin", "mesh fixture");
        REQUIRE(doc->create_entity(parent).has_value());
        REQUIRE(doc->create_entity(child).has_value());
        REQUIRE(doc->set_name(parent, "父节点😀").has_value());
        REQUIRE(doc->set_name(child, "child").has_value());
        dk::Trsd a;
        a.translation = {10, 0, 0};
        a.scale = {2, 3, -1};
        a.rotation = *dk::rotation_from_axis_angle(dk::Vec3d{0, 0, 1}, dk::radians(30.0));
        dk::Trsd b;
        b.translation = {1, 2, 3};
        b.rotation = *dk::rotation_from_axis_angle(dk::Vec3d{0, 0, 1}, dk::radians(45.0));
        REQUIRE(doc->set_local_transform(parent, a).has_value());
        REQUIRE(doc->set_local_transform(child, b).has_value());
        REQUIRE(doc->set_parent(child, parent).has_value());
        REQUIRE(doc->set_asset_references(child, {{asset, dk::AssetKind::mesh}}).has_value());
    }
    std::string text() { return *dk::serialize_scene(*doc->snapshot(), project); }
};
Json& component(Json& json, std::size_t index, std::string_view name)
{
    return json["entities"][index]["components"][name];
}
std::string read(const std::filesystem::path& path)
{
    const auto bytes = dk::read_file_bytes(path);
    REQUIRE(bytes.has_value());
    if (bytes->empty()) { return {}; }
    return {reinterpret_cast<const char*>(bytes->data()), bytes->size()};
}
void no_temporaries(const std::filesystem::path& root)
{
    for (const auto& entry : std::filesystem::directory_iterator(root)) {
        REQUIRE_FALSE(entry.path().filename().string().starts_with(".dk-save-"));
    }
}
}

TEST_CASE("scene files roundtrip identity hierarchy affine worlds assets and clean revisions", "[scene][persistence]")
{
    Fixture fixture;
    const auto snapshot = fixture.doc->snapshot();
    REQUIRE(snapshot.has_value());
    REQUIRE(snapshot->entities()[0].id == fixture.child); // Child precedes parent in file.
    REQUIRE(dk::save_project(fixture.project, "project.json").has_value());
    REQUIRE(dk::save_scene(*fixture.doc, fixture.project).has_value());
    REQUIRE_FALSE(fixture.doc->dirty());
    REQUIRE(fixture.project.validate_files().has_value());
    auto loaded = dk::load_scene(fixture.project);
    REQUIRE(loaded.has_value());
    REQUIRE_FALSE((*loaded)->dirty());
    REQUIRE((*loaded)->revision() == snapshot->revision());
    REQUIRE((*loaded)->id() == fixture.doc->id());
    REQUIRE((*loaded)->entity_ids() == fixture.doc->entity_ids());
    REQUIRE((*loaded)->entity(fixture.child)->parent == fixture.parent);
    REQUIRE((*loaded)->entity(fixture.child)->assets == fixture.doc->entity(fixture.child)->assets);
    REQUIRE((*loaded)->entity(fixture.parent)->name == "父节点😀");
    REQUIRE((*loaded)->world_transform(fixture.child)->matrix().isApprox(
        fixture.doc->world_transform(fixture.child)->matrix(), 1e-12));
    REQUIRE((*loaded)->validate().has_value());
    REQUIRE(*dk::serialize_scene(*(*loaded)->snapshot(), fixture.project) == read(fixture.files.root / "scene.json"));
    const auto loaded_revision = (*loaded)->revision();
    REQUIRE((*loaded)->set_local_transform(fixture.child, (*loaded)->entity(fixture.child)->local).has_value());
    REQUIRE((*loaded)->revision() == loaded_revision);
    REQUIRE((*loaded)->set_name(fixture.parent, "父节点😀").has_value());
    REQUIRE_FALSE((*loaded)->dirty());
    const auto imported = dk::parse_scene(fixture.text(), fixture.project);
    REQUIRE(imported.has_value());
    REQUIRE((*imported)->dirty());
    no_temporaries(fixture.files.root);
}

TEST_CASE("scene stale snapshots save their captured data without clearing later edits", "[scene][persistence]")
{
    Fixture fixture;
    const auto old = fixture.doc->snapshot();
    REQUIRE(old.has_value());
    const auto old_revision = old->revision();
    REQUIRE(fixture.doc->set_name(fixture.child, "new edit").has_value());
    REQUIRE(dk::save_scene(*fixture.doc, *old, fixture.project).has_value());
    REQUIRE(fixture.doc->dirty());
    REQUIRE((*dk::load_scene(fixture.project))->revision() == old_revision);
    REQUIRE((*dk::load_scene(fixture.project))->entity(fixture.child)->name == "child");
    REQUIRE(fixture.doc->entity(fixture.child)->name == "new edit");
    REQUIRE(dk::save_scene(*fixture.doc, fixture.project).has_value());
    REQUIRE_FALSE(fixture.doc->dirty());
    REQUIRE(dk::save_scene(*fixture.doc, *old, fixture.project).has_value());
    REQUIRE(fixture.doc->dirty()); // An older overwrite must dirty even an already-clean document.
    REQUIRE(dk::reload_scene(fixture.doc, fixture.project).has_value());
    REQUIRE_FALSE(fixture.doc->dirty());
    const auto before = read(fixture.files.root / "scene.json");
    REQUIRE_FALSE(dk::save_scene(*fixture.doc, *old, fixture.project).has_value());
    REQUIRE(read(fixture.files.root / "scene.json") == before);
    REQUIRE_FALSE(fixture.doc->dirty());
}

TEST_CASE("scene snapshots from matching scene IDs cannot acknowledge another document instance", "[scene][persistence]")
{
    Fixture fixture;
    const auto snapshot = fixture.doc->snapshot();
    auto other = *dk::SceneDocument::create(fixture.doc->id());
    REQUIRE_FALSE(dk::save_scene(*other, *snapshot, fixture.project).has_value());
    REQUIRE(other->dirty());
    REQUIRE_FALSE(std::filesystem::exists(fixture.files.root / "scene.json"));
}

TEST_CASE("scene invalid schemas and references never replace a live document", "[scene][persistence]")
{
    Fixture fixture;
    REQUIRE(dk::save_scene(*fixture.doc, fixture.project).has_value());
    const auto base = Json::parse(fixture.text());
    REQUIRE(fixture.doc->set_name(fixture.parent, "unsaved").has_value());
    const auto pointer = fixture.doc.get();
    const auto revision = fixture.doc->revision();
    std::vector<Json> invalid;
    auto add = [&](auto change) { auto value = base; change(value); invalid.push_back(std::move(value)); };
    add([](Json& j) { j["version"] = 2; });
    add([](Json& j) { j["revision"] = -1; });
    add([](Json& j) { j["revision"] = 1.5; });
    add([](Json& j) { j["scene_id"] = "not-a-uuid"; });
    add([](Json& j) { j["entities"].push_back(j["entities"][0]); });
    add([](Json& j) { component(j, 0, "dk.Name")["version"] = 2; });
    add([](Json& j) { component(j, 0, "dk.Name")["extra"] = 1; });
    add([](Json& j) { component(j, 0, "dk.Transform")["rotation"] = {0, 0, 0, 0}; });
    add([](Json& j) { component(j, 0, "dk.Transform")["translation"] = {1, 2}; });
    add([](Json& j) { component(j, 0, "dk.Transform")["scale"] = {true, 1, 1}; });
    add([](Json& j) { j["entities"][0]["components"].erase("dk.Name"); });
    add([](Json& j) { j["entities"][0]["components"]["future.Component"] = {}; });
    add([&](Json& j) { component(j, 1, "dk.Hierarchy")["parent"] = fixture.child.to_string(); });
    add([&](Json& j) { component(j, 0, "dk.Hierarchy")["parent"] = fixture.child.to_string(); });
    add([](Json& j) { component(j, 0, "dk.Hierarchy")["parent"] = dk::EntityId::generate()->to_string(); });
    add([](Json& j) { component(j, 0, "dk.Hierarchy")["parent"] = dk::EntityId{}.to_string(); });
    add([](Json& j) { component(j, 0, "dk.AssetReferences")["items"][0]["kind"] = "texture"; });
    add([](Json& j) { component(j, 0, "dk.AssetReferences")["items"][0]["id"] = dk::AssetId::generate()->to_string(); });
    add([](Json& j) { auto& items = component(j, 0, "dk.AssetReferences")["items"]; items.push_back(items[0]); });
    add([](Json& j) { j["entities"] = Json::array_t(10001, Json::object()); });
    for (const auto& json : invalid) {
        const auto text = json.dump();
        fixture.files.write("scene.json", text);
        REQUIRE_FALSE(dk::reload_scene(fixture.doc, fixture.project).has_value());
        REQUIRE(fixture.doc.get() == pointer);
        REQUIRE(fixture.doc->revision() == revision);
        REQUIRE(fixture.doc->dirty());
        REQUIRE(fixture.doc->entity(fixture.parent)->name == "unsaved");
        REQUIRE(read(fixture.files.root / "scene.json") == text);
    }
    for (const auto& text : {std::string{}, std::string{"{"}, base.dump() + "{}",
        std::string{"{\"format\":\"DeckerScene\",\"format\":\"DeckerScene\"}"}}) {
        fixture.files.write("scene.json", text);
        REQUIRE_FALSE(dk::reload_scene(fixture.doc, fixture.project).has_value());
        REQUIRE(fixture.doc.get() == pointer);
    }
}

TEST_CASE("scene absent assets and overflow preserve loaded document and saved bytes", "[scene][persistence]")
{
    Fixture fixture;
    REQUIRE(dk::save_scene(*fixture.doc, fixture.project).has_value());
    const auto old = read(fixture.files.root / "scene.json");
    const auto pointer = fixture.doc.get();
    REQUIRE(std::filesystem::remove(fixture.files.root / "mesh.bin"));
    REQUIRE_FALSE(dk::reload_scene(fixture.doc, fixture.project).has_value());
    REQUIRE(fixture.doc.get() == pointer);
    REQUIRE_FALSE(fixture.doc->dirty());
    REQUIRE_FALSE(dk::save_scene(*fixture.doc, fixture.project).has_value());
    REQUIRE(read(fixture.files.root / "scene.json") == old);
    fixture.files.write("mesh.bin", "restored");
    auto json = Json::parse(old);
    component(json, 1, "dk.Transform")["scale"] = {std::numeric_limits<double>::max(), 1, 1};
    component(json, 0, "dk.Transform")["scale"] = {4, 1, 1};
    REQUIRE_FALSE(dk::parse_scene(json.dump(), fixture.project).has_value());
    no_temporaries(fixture.files.root);
}

TEST_CASE("scene maximum revision loads safely and rejects further edits without wraparound", "[scene][persistence]")
{
    Fixture fixture;
    auto json = Json::parse(fixture.text());
    json["revision"] = std::numeric_limits<std::uint64_t>::max();
    fixture.files.write("scene.json", json.dump());
    auto loaded = dk::load_scene(fixture.project);
    REQUIRE(loaded.has_value());
    REQUIRE_FALSE((*loaded)->dirty());
    REQUIRE_FALSE((*loaded)->create_entity().has_value());
    REQUIRE_FALSE((*loaded)->set_name(fixture.child, "changed").has_value());
    REQUIRE_FALSE((*loaded)->destroy_entity(fixture.child).has_value());
    REQUIRE((*loaded)->revision() == std::numeric_limits<std::uint64_t>::max());
    REQUIRE_FALSE((*loaded)->dirty());
    REQUIRE((*loaded)->validate().has_value());
}

TEST_CASE("atomic file validation can reject or throw without replacing old content", "[scene][persistence]")
{
#ifdef _WIN32
    SceneTestFiles files;
    files.write("value.txt", "old");
    const auto target = files.root / "value.txt";
    const std::string value = "new";
    const auto bytes = std::as_bytes(std::span{value.data(), value.size()});
    bool called = false;
    const auto result = dk::write_file_bytes_atomic(target, bytes, [&](const std::filesystem::path& temporary) -> dk::Result<void> {
        called = true;
        REQUIRE(temporary != target);
        REQUIRE(read(temporary) == value);
        REQUIRE(read(target) == "old");
        return std::unexpected(dk::Error{dk::ErrorCode::invalid_argument, "reject fixture", {}});
    });
    REQUIRE(called);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(read(target) == "old");
    no_temporaries(files.root);
    REQUIRE_THROWS_AS(dk::write_file_bytes_atomic(target, bytes,
        [](const std::filesystem::path&) -> dk::Result<void> { throw std::runtime_error("fixture"); }), std::runtime_error);
    REQUIRE(read(target) == "old");
    no_temporaries(files.root);
#else
    SKIP("Atomic replacement is Windows-only");
#endif
}

TEST_CASE("scene sharing conflicts preserve old file revision dirty state and clean temporary files", "[scene][persistence]")
{
#ifdef _WIN32
    Fixture fixture;
    REQUIRE(dk::save_scene(*fixture.doc, fixture.project).has_value());
    const auto path = fixture.files.root / "scene.json";
    const auto before = read(path);
    REQUIRE(fixture.doc->set_name(fixture.child, "unsaved edit").has_value());
    const auto revision = fixture.doc->revision();
    const auto handle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    REQUIRE(handle != INVALID_HANDLE_VALUE);
    const auto saved = dk::save_scene(*fixture.doc, fixture.project);
    REQUIRE(CloseHandle(handle));
    REQUIRE_FALSE(saved.has_value());
    REQUIRE(saved.error().code == dk::ErrorCode::io_error);
    REQUIRE(fixture.doc->revision() == revision);
    REQUIRE(fixture.doc->dirty());
    REQUIRE(read(path) == before);
    no_temporaries(fixture.files.root);
    REQUIRE(dk::save_scene(*fixture.doc, fixture.project).has_value());
    REQUIRE_FALSE(fixture.doc->dirty());
#else
    SKIP("Sharing conflict fixture is Windows-only");
#endif
}

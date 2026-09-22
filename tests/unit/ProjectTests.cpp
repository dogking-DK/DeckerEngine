#include <dk/scene/Project.hpp>
#include <dk/scene/SceneDocument.hpp>
#include "SceneTestFiles.hpp"
#include <algorithm>

namespace {
dk::ProjectDescription description()
{
    return {"工程😀", "scenes/main.json",
        {{*dk::AssetId::parse("00112233-4455-4677-8899-aabbccddeeff"), dk::AssetKind::mesh, "assets/mesh.bin"}}};
}
std::string replace(std::string text, std::string_view from, std::string_view to)
{
    const auto position = text.find(from);
    REQUIRE(position != std::string::npos);
    text.replace(position, from.size(), to);
    return text;
}
}

TEST_CASE("project protocol roundtrips Unicode and emits deterministically sorted asset records", "[scene][project]")
{
    auto data = description();
    data.assets.push_back({*dk::AssetId::generate(), dk::AssetKind::texture, "assets/纹理.png"});
    const auto text = dk::serialize_project(data);
    REQUIRE(text.has_value());
    const auto parsed = dk::parse_project(*text);
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->name == data.name);
    REQUIRE(parsed->scene == data.scene);
    REQUIRE(parsed->assets.size() == 2);
    REQUIRE(*dk::serialize_project(*parsed) == *text);
    std::reverse(data.assets.begin(), data.assets.end());
    REQUIRE(*dk::serialize_project(data) == *text);
}

TEST_CASE("project rejects malformed schemas versions duplicate keys and IDs", "[scene][project]")
{
    const auto text = *dk::serialize_project(description());
    for (const auto& bad : {
        std::string{"{}"}, std::string{"["}, text + "{}",
        replace(text, "\"version\": 1", "\"version\": 2"),
        replace(text, "\"version\": 1", "\"version\": 1.0"),
        replace(text, "\"version\": 1", "\"version\": true"),
        replace(text, "\"version\": 1", "\"version\": 1, \"version\": 1"),
        replace(text, "\"version\": 1", "\"version\": 1, \"extra\": 1"),
        replace(text, "DeckerProject", "Other"),
        replace(text, "mesh\"", "unknown\""),
        replace(text, "00112233-4455-4677-8899-aabbccddeeff", "00000000-0000-0000-0000-000000000000"),
        replace(text, "00112233-4455-4677-8899-aabbccddeeff", "bad-id")
    }) { REQUIRE_FALSE(dk::parse_project(bad).has_value()); }
    const auto newer = dk::parse_project(replace(text, "\"version\": 1", "\"version\": 2"));
    REQUIRE(newer.error().code == dk::ErrorCode::not_supported);
    auto data = description();
    data.assets.push_back(data.assets.front());
    REQUIRE_FALSE(dk::serialize_project(data).has_value());
    auto records = text.substr(text.find("{", text.find("\"assets\"") + 8));
    records.resize(records.find("}") + 1);
    const auto duplicate = replace(text, records, records + "," + records);
    REQUIRE_FALSE(dk::parse_project(duplicate).has_value());
}

TEST_CASE("project bounds strings paths JSON size and nesting", "[scene][project]")
{
    for (const auto path : {"../scene.json", "/scene.json", "C:/scene.json", "a/../b", "a/./b", "a//b", "a\\b", ""}) {
        auto data = description();
        data.scene = path;
        REQUIRE_FALSE(dk::serialize_project(data).has_value());
    }
    auto data = description();
    data.name = std::string("x\0y", 3);
    REQUIRE_FALSE(dk::serialize_project(data).has_value());
    data.name = std::string("\xff", 1);
    REQUIRE_FALSE(dk::serialize_project(data).has_value());
    data.name = std::string(1025, 'x');
    REQUIRE_FALSE(dk::serialize_project(data).has_value());
    REQUIRE_FALSE(dk::parse_project(std::string(16U * 1024U * 1024U + 1, ' ')).has_value());
    REQUIRE_FALSE(dk::parse_project(std::string(100, '[') + "0" + std::string(100, ']')).has_value());
}

TEST_CASE("project opens a manifest and resolves assets relative to captured Unicode root", "[scene][project]")
{
    SceneTestFiles files;
    const auto data = description();
    files.write("config/project.json", *dk::serialize_project(data));
    files.write(data.scene, "{}");
    files.write(data.assets[0].path, "mesh payload is not decoded");
    const auto project = dk::Project::open(files.root, "config/project.json");
    REQUIRE(project.has_value());
    REQUIRE(project->validate_files().has_value());
    const auto asset = project->resolve_asset({data.assets[0].id, dk::AssetKind::mesh});
    REQUIRE(asset.has_value());
    REQUIRE(*asset == std::filesystem::canonical(files.root) / data.assets[0].path);
    REQUIRE_FALSE(dk::Project::open(files.root, "../outside.json").has_value());
    REQUIRE_FALSE(dk::Project::open(files.root, "missing.json").has_value());
}

TEST_CASE("project diagnostics distinguish unregistered mismatched missing and directory assets", "[scene][project]")
{
    SceneTestFiles files;
    const auto data = description();
    const auto project = dk::Project::create(files.root, data);
    REQUIRE(project.has_value());
    const auto reference = dk::AssetReference{data.assets[0].id, dk::AssetKind::mesh};
    const auto missing = project->resolve_asset(reference);
    REQUIRE_FALSE(missing.has_value());
    REQUIRE(missing.error().code == dk::ErrorCode::not_found);
    REQUIRE_FALSE(missing.error().context.empty());
    REQUIRE(project->resolve_asset({*dk::AssetId::generate(), dk::AssetKind::mesh}).error().code == dk::ErrorCode::not_found);
    REQUIRE(project->resolve_asset({reference.id, dk::AssetKind::texture}).error().code == dk::ErrorCode::invalid_argument);
    REQUIRE_FALSE(project->validate_files().has_value());
    std::filesystem::create_directories(files.root / data.assets[0].path);
    REQUIRE(project->resolve_asset(reference).error().code == dk::ErrorCode::invalid_argument);
}

TEST_CASE("scene asset references validate independently and resolve with entity diagnostics", "[scene][project]")
{
    SceneTestFiles files;
    const auto data = description();
    const auto project = dk::Project::create(files.root, data);
    REQUIRE(project.has_value());
    auto scene = *dk::SceneDocument::create();
    const auto id = *scene->create_entity();
    const auto reference = dk::AssetReference{data.assets[0].id, dk::AssetKind::mesh};
    REQUIRE(scene->set_asset_references(id, {reference}).has_value());
    const auto revision = scene->revision();
    REQUIRE(scene->set_asset_references(id, {reference}).has_value());
    REQUIRE_FALSE(scene->set_asset_references(id, {reference, reference}).has_value());
    REQUIRE_FALSE(scene->set_asset_references(id, {{dk::AssetId{}, dk::AssetKind::mesh}}).has_value());
    REQUIRE_FALSE(scene->set_asset_references(id, {{reference.id, static_cast<dk::AssetKind>(99)}}).has_value());
    REQUIRE_FALSE(scene->set_asset_references(id, std::vector<dk::AssetReference>(65, reference)).has_value());
    REQUIRE(scene->revision() == revision);
    const auto missing = dk::check_asset_references(*scene, *project);
    REQUIRE_FALSE(missing.has_value());
    REQUIRE(missing.error().context.back() == id.to_string());
    REQUIRE(scene->revision() == revision);
    files.write(data.assets[0].path, "asset");
    REQUIRE(dk::check_asset_references(*scene, *project).has_value());
    REQUIRE(scene->validate().has_value());
}

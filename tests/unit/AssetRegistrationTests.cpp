#include "AssetTestSupport.hpp"
#include <dk/services/AssetRegistration.hpp>
#include <dk/scene/SceneIO.hpp>

TEST_CASE("project registration adopts legacy identity without modifying the original project or scene", "[assets][project]")
{
    AssetTestMemory memory; SceneTestFiles files; write_test_source(files);
    const auto id = *dk::AssetId::generate();
    dk::ProjectDescription description{"工程😀", "scenes/main.json", {{id, dk::AssetKind::mesh, "assets/模型.gltf"}}};
    files.write("project.json", *dk::serialize_project(description)); files.write("scenes/main.json", "{}");
    auto project = dk::Project::open(files.root, "project.json").value(); REQUIRE(project.validate_files());
    auto scene = dk::SceneDocument::create().value(); const auto entity = scene->create_entity().value();
    REQUIRE(scene->set_asset_references(entity, {{id, dk::AssetKind::mesh}}));
    const auto revision = scene->revision(); const auto dirty = scene->dirty();
    auto catalog = dk::make_asset_catalog(project); REQUIRE(catalog);
    const auto old = *dk::serialize_project(project.description());
    const dk::AssetOutputSpec outputs[]{{"mesh/0", dk::AssetKind::mesh}, {"material/0", dk::AssetKind::material}};
    auto request = first_registration(); request.outputs = outputs;
    const auto prepared = dk::prepare_asset_registration(project, *catalog, catalog->guard(), request); REQUIRE(prepared);
    REQUIRE(prepared->registration.metadata().root_id == id); REQUIRE(prepared->project.description().assets.size() == 2);
    REQUIRE(prepared->project.description().name == description.name); REQUIRE(prepared->project.description().scene == description.scene);
    REQUIRE(*dk::serialize_project(project.description()) == old); REQUIRE(catalog->records().size() == 1);
    REQUIRE(dk::check_asset_references(*scene, prepared->project)); REQUIRE(scene->revision() == revision); REQUIRE(scene->dirty() == dirty);
    REQUIRE_FALSE(std::filesystem::exists(files.root / *dk::path_from_utf8("assets/模型.gltf.meta")));
    REQUIRE(dk::Project::open(files.root, "project.json")->description().assets.size() == 1);
}

TEST_CASE("project candidate repeats persisted mappings and keeps all paths on the source", "[assets][project]")
{
    AssetTestMemory memory; SceneTestFiles files; write_test_source(files);
    auto project = dk::Project::create(files.root, {"test", "main.json", {}}).value();
    auto catalog = dk::make_asset_catalog(project).value();
    const auto first = dk::prepare_asset_registration(project, catalog, catalog.guard(), first_registration()); REQUIRE(first);
    write_test_meta(files, first->registration.source(), first->registration.metadata());
    auto reloaded = dk::Project::create(files.root, dk::parse_project(*dk::serialize_project(first->project.description())).value()).value();
    auto next = dk::make_asset_catalog(reloaded).value();
    const auto repeat = dk::prepare_asset_registration(reloaded, next, next.guard(), {"assets/模型.gltf"}); REQUIRE(repeat);
    REQUIRE_FALSE(repeat->registration.changed());
    REQUIRE(repeat->registration.metadata().root_id == first->registration.metadata().root_id);
    REQUIRE(repeat->project.description().assets.front().path == "assets/模型.gltf");
}

TEST_CASE("project adapter rejects mismatched snapshots and failed candidates without changing either input", "[assets][project]")
{
    AssetTestMemory memory; SceneTestFiles files; write_test_source(files);
    auto project = dk::Project::create(files.root, {"test", "main.json", {}}).value();
    auto catalog = dk::make_asset_catalog(project).value();
    auto changed = project.description(); changed.assets.push_back({*dk::AssetId::generate(), dk::AssetKind::mesh, "assets/模型.gltf"});
    auto different = dk::Project::create(files.root, changed).value();
    REQUIRE_FALSE(dk::prepare_asset_registration(different, catalog, catalog.guard(), first_registration()));
    auto matching = dk::make_asset_catalog(different).value(); changed.assets[0].path = "assets/other.glb";
    auto wrong_path = dk::Project::create(files.root, changed).value();
    REQUIRE_FALSE(dk::prepare_asset_registration(wrong_path, matching, matching.guard(), first_registration()));
    SceneTestFiles other_files; auto wrong_root = dk::Project::create(other_files.root, project.description()).value();
    REQUIRE_FALSE(dk::prepare_asset_registration(wrong_root, catalog, catalog.guard(), first_registration()));
    files.write(*dk::path_from_utf8("assets/模型.gltf.meta"), "damaged");
    REQUIRE_FALSE(dk::prepare_asset_registration(project, catalog, catalog.guard(), first_registration()));
    REQUIRE(project.description().assets.empty()); REQUIRE(catalog.records().empty()); REQUIRE(catalog.guard().revision == 0);
}

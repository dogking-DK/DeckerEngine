#include "AssetTestSupport.hpp"
#include "AssetPersistenceInternal.hpp"
#include <dk/services/AssetService.hpp>
#include <dk/scene/SceneIO.hpp>
#include <catch2/generators/catch_generators.hpp>

TEST_CASE("asset service commits registration rename and no-op while preserving IDs and scene", "[assets][service]")
{
    AssetTestMemory memory; SceneTestFiles files; write_test_source(files);
    const auto id = *dk::AssetId::generate();
    files.write("project.json", *dk::serialize_project({"工程😀", "scene.json", {{id, dk::AssetKind::mesh, "assets/模型.gltf"}}}));
    auto service = dk::AssetService::open(files.root, "project.json").value();
    auto scene = dk::SceneDocument::create().value(); const auto entity = scene->create_entity().value();
    REQUIRE(scene->set_asset_references(entity, {{id, dk::AssetKind::mesh}}));
    const auto revision = scene->revision(); const auto dirty = scene->dirty();
    const auto snapshot = dk::serialize_scene(*scene->snapshot(), service.project()).value();
    const dk::AssetOutputSpec outputs[]{{"mesh/0", dk::AssetKind::mesh}, {"material/0", dk::AssetKind::material}, {"texture/0", dk::AssetKind::texture}};
    auto request = first_registration(); request.outputs = outputs;
    const auto initial = service.catalog().guard();
    REQUIRE(service.register_source(initial, request)); REQUIRE(service.catalog().guard().revision == 1);
    REQUIRE(service.catalog().inspect_source(request.source)->root_id == id); REQUIRE(service.project().description().assets.size() == 3);
    const auto records = service.project().description().assets;
    const auto meta_path = files.root / *dk::path_from_utf8("assets/模型.gltf.meta");
    const auto raw = dk::read_file_bytes(meta_path).value(); const auto modified = std::filesystem::last_write_time(meta_path);
    const auto* project = &service.project();
    REQUIRE(service.register_source(service.catalog().guard(), {request.source}));
    REQUIRE(service.catalog().guard().revision == 1); REQUIRE(&service.project() == project);
    REQUIRE(std::filesystem::last_write_time(meta_path) == modified);
    REQUIRE_FALSE(service.register_source(initial, request));
    REQUIRE(service.rename_source(service.catalog().guard(), request.source, "assets/新名称.gltf"));
    REQUIRE(service.catalog().guard().revision == 2); REQUIRE_FALSE(std::filesystem::exists(meta_path));
    REQUIRE(dk::read_file_bytes(files.root / *dk::path_from_utf8("assets/新名称.gltf.meta")).value() == raw);
    for (std::size_t i = 0; i < records.size(); ++i) {
        const auto& value = service.project().description().assets[i];
        REQUIRE(value.id == records[i].id); REQUIRE(value.kind == records[i].kind); REQUIRE(value.path == "assets/新名称.gltf");
    }
    REQUIRE(dk::serialize_scene(*scene->snapshot(), service.project()).value() == snapshot);
    REQUIRE(scene->revision() == revision); REQUIRE(scene->dirty() == dirty);
    auto reopened = dk::AssetService::open(files.root, "project.json").value();
    REQUIRE(reopened.catalog().guard().session_id != service.catalog().guard().session_id);
    REQUIRE(reopened.catalog().guard().revision == 0); REQUIRE(reopened.project().description().assets == service.project().description().assets);
    REQUIRE(reopened.catalog().inspect_source("assets/新名称.gltf")->root_id == id);
}
TEST_CASE("asset service failures preserve Project and require reopen after recovery", "[assets][service]")
{
    AssetTestMemory memory; SceneTestFiles files; write_test_source(files);
    files.write("project.json", *dk::serialize_project({"test", "scene.json", {}}));
    auto service = dk::AssetService::open(files.root, "project.json").value(); const auto* original = &service.project();
    using namespace dk::asset_detail;
    {
        OperationHook hook = [](OperationStep step) -> dk::Result<void> {
            if (step == OperationStep::finish || step == OperationStep::rollback_meta) {
                return std::unexpected(dk::Error{dk::ErrorCode::io_error, "failure"});
            } return {};
        };
        ScopedOperationHook scope{hook}; REQUIRE_FALSE(service.register_source(service.catalog().guard(), first_registration()));
    }
    REQUIRE(&service.project() == original); REQUIRE(service.project().description().assets.empty());
    REQUIRE(service.catalog().guard().revision == 0); REQUIRE(service.catalog().needs_recovery());
    REQUIRE_FALSE(dk::AssetService::open(files.root, "project.json"));
    REQUIRE_FALSE(service.register_source(service.catalog().guard(), first_registration()));
    REQUIRE(dk::recover_asset_operations(files.root));
    REQUIRE_FALSE(service.register_source(service.catalog().guard(), first_registration()));
    auto reopened = dk::AssetService::open(files.root, "project.json").value();
    REQUIRE(reopened.register_source(reopened.catalog().guard(), first_registration()));
}
TEST_CASE("asset service incremental registration reuses IDs and checks external manifest even for no-op", "[assets][service]")
{
    AssetTestMemory memory; SceneTestFiles files; write_test_source(files);
    files.write("project.json", *dk::serialize_project({"test", "scene.json", {}}));
    auto service = dk::AssetService::open(files.root, "project.json").value();
    REQUIRE(service.register_source(service.catalog().guard(), first_registration()));
    const auto root = service.catalog().records().front().id;
    const dk::AssetOutputSpec outputs[]{{"mesh/0", dk::AssetKind::mesh}, {"material/5", dk::AssetKind::material}};
    REQUIRE(service.register_source(service.catalog().guard(), {"assets/模型.gltf", outputs, 2.5}));
    REQUIRE(service.catalog().guard().revision == 2); REQUIRE(service.catalog().records().size() == 2);
    const auto meta = service.catalog().inspect_source("assets/模型.gltf").value(); REQUIRE(meta.root_id == root); REQUIRE(meta.unit_scale == 2.5);
    files.write("project.json", "external bytes");
    REQUIRE_FALSE(service.register_source(service.catalog().guard(), {"assets/模型.gltf"}));
    REQUIRE(service.catalog().guard().revision == 2); REQUIRE(service.project().description().assets.size() == 2);
    REQUIRE(dk::check_asset_operations(files.root));
}
TEST_CASE("asset service refuses to move or overwrite Scene-owned paths", "[assets][service]")
{
    const auto scene_path = GENERATE("assets/模型.gltf.meta", "assets/模型.gltf", "assets/new.gltf", "assets/new.gltf.meta");
    AssetTestMemory memory; SceneTestFiles files; write_test_source(files);
    files.write("project.json", *dk::serialize_project({"test", scene_path, {}}));
    auto service = dk::AssetService::open(files.root, "project.json").value();
    if (std::string_view{scene_path}.starts_with("assets/模型")) {
        REQUIRE_FALSE(service.register_source(service.catalog().guard(), first_registration()));
        REQUIRE(service.catalog().records().empty());
    } else {
        REQUIRE(service.register_source(service.catalog().guard(), first_registration()));
        REQUIRE_FALSE(service.rename_source(service.catalog().guard(), "assets/模型.gltf", "assets/new.gltf"));
        REQUIRE(service.catalog().guard().revision == 1);
    }
    REQUIRE(std::filesystem::exists(files.root / *dk::path_from_utf8("assets/模型.gltf")));
    REQUIRE(dk::check_asset_operations(files.root));
}

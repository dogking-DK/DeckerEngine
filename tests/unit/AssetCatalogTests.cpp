#include "AssetTestSupport.hpp"
#include <algorithm>

TEST_CASE("catalog first registration prepares owning values without writing files or publishing state", "[assets]")
{
    AssetTestMemory memory; SceneTestFiles files; write_test_source(files);
    auto catalog = dk::AssetCatalog::create(files.root).value();
    const auto guard = catalog.guard(); REQUIRE_FALSE(guard.session_id.is_nil());
    const auto missing = catalog.inspect_source("assets/模型.gltf"); REQUIRE_FALSE(missing); REQUIRE(missing.error().code == dk::ErrorCode::not_found);
    REQUIRE_FALSE(catalog.prepare_registration(guard, {"assets/模型.gltf"}));
    const auto prepared = catalog.prepare_registration(guard, first_registration()); REQUIRE(prepared);
    REQUIRE(prepared->changed()); REQUIRE(prepared->next_guard().revision == 1); REQUIRE(prepared->next_guard().session_id == guard.session_id);
    REQUIRE(prepared->records().size() == 1); REQUIRE(prepared->metadata().root_id == prepared->records()[0].id);
    REQUIRE_FALSE(prepared->expected_meta_bytes()); REQUIRE(catalog.validate_registration(*prepared));
    REQUIRE(catalog.guard() == guard); REQUIRE(catalog.records().empty());
    REQUIRE_FALSE(std::filesystem::exists(files.root / *dk::path_from_utf8("assets/模型.gltf.meta")));
    write_test_meta(files, prepared->source(), prepared->metadata()); // Test fixture simulates a later durable stage.
    REQUIRE(catalog.validate_registration(*prepared).error().code == dk::ErrorCode::conflict);
}

TEST_CASE("catalog repeated and incremental registration preserves all existing mappings", "[assets]")
{
    AssetTestMemory memory; SceneTestFiles files; write_test_source(files);
    auto meta = test_meta(); meta.unit_scale = 0.5;
    const auto texture = *dk::AssetId::generate(); meta.outputs.push_back({"texture/2", texture, dk::AssetKind::texture});
    write_test_meta(files, "assets/模型.gltf", meta);
    const dk::AssetCatalogRecord records[]{{meta.root_id, dk::AssetKind::mesh, "assets/模型.gltf"}, {texture, dk::AssetKind::texture, "assets/模型.gltf"}};
    auto catalog = dk::AssetCatalog::create(files.root, records).value();
    const auto same = catalog.prepare_registration(catalog.guard(), {"assets/模型.gltf"}); REQUIRE(same);
    REQUIRE_FALSE(same->changed()); REQUIRE(same->metadata().unit_scale == 0.5); REQUIRE(same->records().size() == 2);
    REQUIRE(catalog.validate_registration(*same));
    const dk::AssetOutputSpec outputs[]{{"mesh/0", dk::AssetKind::mesh}, {"material/0", dk::AssetKind::material}};
    const auto added = catalog.prepare_registration(catalog.guard(), {"assets/模型.gltf", outputs, 2.0}); REQUIRE(added);
    REQUIRE(added->changed()); REQUIRE(added->metadata().root_id == meta.root_id); REQUIRE(added->metadata().unit_scale == 2);
    REQUIRE(added->metadata().outputs.size() == 3); REQUIRE(added->records().size() == 3);
    REQUIRE(std::any_of(added->metadata().outputs.begin(), added->metadata().outputs.end(), [&](const auto& item) { return item.id == texture; }));
    REQUIRE(catalog.inspect_source("assets/模型.gltf")->unit_scale == 0.5); REQUIRE(catalog.records().size() == 2);
}

TEST_CASE("catalog adopts only unambiguous legacy mesh identity explicitly", "[assets]")
{
    AssetTestMemory memory; SceneTestFiles files; write_test_source(files);
    dk::Vector<dk::AssetCatalogRecord> records{{*dk::AssetId::generate(), dk::AssetKind::mesh, "assets/模型.gltf"}};
    auto catalog = dk::AssetCatalog::create(files.root, records).value();
    REQUIRE_FALSE(catalog.prepare_registration(catalog.guard(), {"assets/模型.gltf"}));
    REQUIRE(catalog.prepare_registration(catalog.guard(), first_registration())->metadata().root_id == records[0].id);
    records[0].kind = dk::AssetKind::texture;
    auto wrong = dk::AssetCatalog::create(files.root, records).value();
    REQUIRE(wrong.prepare_registration(wrong.guard(), first_registration()).error().code == dk::ErrorCode::conflict);
    records[0].kind = dk::AssetKind::mesh; records.push_back({*dk::AssetId::generate(), dk::AssetKind::mesh, "assets/模型.gltf"});
    auto ambiguous = dk::AssetCatalog::create(files.root, records).value();
    REQUIRE(ambiguous.prepare_registration(ambiguous.guard(), first_registration()).error().code == dk::ErrorCode::conflict);
}

TEST_CASE("catalog refuses damaged missing directory and unsupported sidecar inputs", "[assets]")
{
    AssetTestMemory memory; SceneTestFiles files; write_test_source(files);
    auto catalog = dk::AssetCatalog::create(files.root).value();
    for (const auto content : {"", "{}", "{broken"}) {
        files.write(*dk::path_from_utf8("assets/模型.gltf.meta"), content);
        REQUIRE_FALSE(catalog.prepare_registration(catalog.guard(), first_registration()));
        REQUIRE(catalog.records().empty()); REQUIRE(catalog.guard().revision == 0);
        const auto bytes = dk::read_file_bytes(files.root / *dk::path_from_utf8("assets/模型.gltf.meta")); REQUIRE(bytes);
        REQUIRE(bytes->size() == std::string_view{content}.size());
    }
    REQUIRE(catalog.inspect_source("assets/missing.glb").error().code == dk::ErrorCode::not_found);
    files.write("assets/model.bin", "data"); REQUIRE_FALSE(catalog.inspect_source("assets/model.bin"));
    files.write("assets/no-extension", "data"); REQUIRE(catalog.inspect_source("assets/no-extension").error().code == dk::ErrorCode::not_supported);
    std::filesystem::create_directories(files.root / "assets/directory.glb"); REQUIRE_FALSE(catalog.inspect_source("assets/directory.glb"));
    write_test_source(files, "assets/directory-meta.glb"); std::filesystem::create_directories(files.root / "assets/directory-meta.glb.meta");
    REQUIRE_FALSE(catalog.inspect_source("assets/directory-meta.glb"));
    files.write("assets/directory-meta.glb.meta/kept", "owned by fixture");
    write_test_source(files, "assets/large.glb"); files.write("assets/large.glb.meta", std::string(dk::asset_meta_byte_limit + 1, ' '));
    REQUIRE_FALSE(catalog.inspect_source("assets/large.glb"));
}

TEST_CASE("catalog conflicts on existing IDs kinds and source paths", "[assets]")
{
    AssetTestMemory memory; SceneTestFiles files; write_test_source(files);
    const auto meta = test_meta(); write_test_meta(files, "assets/模型.gltf", meta);
    for (const auto& record : {
        dk::AssetCatalogRecord{meta.root_id, dk::AssetKind::mesh, "assets/other.gltf"},
        dk::AssetCatalogRecord{meta.root_id, dk::AssetKind::texture, "assets/模型.gltf"},
        dk::AssetCatalogRecord{*dk::AssetId::generate(), dk::AssetKind::mesh, "assets/模型.gltf"}}) {
        auto catalog = dk::AssetCatalog::create(files.root, {&record, 1}).value();
        const auto result = catalog.prepare_registration(catalog.guard(), first_registration()); REQUIRE_FALSE(result);
        REQUIRE(result.error().code == dk::ErrorCode::conflict); REQUIRE(catalog.records()[0] == record);
        REQUIRE_FALSE(catalog.inspect_source("assets/模型.gltf"));
    }
}

TEST_CASE("catalog rejects stale sessions revisions and changed sidecar bytes", "[assets]")
{
    AssetTestMemory memory; SceneTestFiles files; write_test_source(files); write_test_meta(files, "assets/模型.gltf", test_meta());
    auto catalog = dk::AssetCatalog::create(files.root).value(); auto other = dk::AssetCatalog::create(files.root).value();
    REQUIRE_FALSE(catalog.prepare_registration(other.guard(), first_registration()));
    auto stale = catalog.guard(); ++stale.revision; REQUIRE_FALSE(catalog.check_guard(stale));
    const auto prepared = catalog.prepare_registration(catalog.guard(), first_registration()); REQUIRE(prepared);
    REQUIRE_FALSE(other.validate_registration(*prepared)); REQUIRE(catalog.validate_registration(*prepared));
    files.write(*dk::path_from_utf8("assets/模型.gltf.meta"), std::string{*prepared->expected_meta_bytes()} + " ");
    REQUIRE(catalog.validate_registration(*prepared).error().code == dk::ErrorCode::conflict);
    std::filesystem::remove(files.root / *dk::path_from_utf8("assets/模型.gltf.meta"));
    REQUIRE(catalog.validate_registration(*prepared).error().code == dk::ErrorCode::conflict);
    std::filesystem::remove(files.root / *dk::path_from_utf8("assets/模型.gltf"));
    REQUIRE(catalog.validate_registration(*prepared).error().code == dk::ErrorCode::not_found);
    REQUIRE(catalog.guard().revision == 0); REQUIRE(catalog.records().empty());
}

TEST_CASE("catalog validates selectors paths duplicates and record budget before publishing", "[assets]")
{
    AssetTestMemory memory; SceneTestFiles files; write_test_source(files);
    auto catalog = dk::AssetCatalog::create(files.root).value();
    for (const auto path : {"../escape.gltf", "a/./b.glb", "a//b.glb", "a\\b.glb", "/a.glb", "C:/a.glb", "a/", ""}) {
        REQUIRE_FALSE(catalog.prepare_registration(catalog.guard(), first_registration(path)));
        const dk::AssetCatalogRecord record{*dk::AssetId::generate(), dk::AssetKind::mesh, path};
        REQUIRE_FALSE(dk::AssetCatalog::create(files.root, {&record, 1}));
    }
    const dk::AssetOutputSpec missing_root[]{{"texture/0", dk::AssetKind::texture}};
    auto request = first_registration(); request.outputs = missing_root; REQUIRE_FALSE(catalog.prepare_registration(catalog.guard(), request));
    const dk::AssetOutputSpec duplicate[]{{"mesh/0", dk::AssetKind::mesh}, {"mesh/0", dk::AssetKind::mesh}};
    request.outputs = duplicate; REQUIRE_FALSE(catalog.prepare_registration(catalog.guard(), request));
    request = first_registration(); request.unit_scale = -1; REQUIRE_FALSE(catalog.prepare_registration(catalog.guard(), request));
    std::string overlong(4092, 'a'); overlong += ".glb";
    REQUIRE_FALSE(catalog.prepare_registration(catalog.guard(), first_registration(overlong)));
    REQUIRE_FALSE(catalog.prepare_registration(catalog.guard(), first_registration(std::string_view{"bad\0.glb", 8})));
    REQUIRE_FALSE(catalog.prepare_registration(catalog.guard(), first_registration(std::string_view{"\xff.glb", 5})));
    dk::Vector<dk::AssetCatalogRecord> records;
    for (std::size_t i = 0; i < dk::asset_catalog_record_limit; ++i) { records.push_back({*dk::AssetId::generate(), dk::AssetKind::texture, "assets/other.gltf"}); }
    auto full = dk::AssetCatalog::create(files.root, records); REQUIRE(full);
    REQUIRE_FALSE(full->prepare_registration(full->guard(), first_registration())); REQUIRE(full->records().size() == 10000);
    records.push_back(records.front()); REQUIRE_FALSE(dk::AssetCatalog::create(files.root, records));
    records.resize(2); records[1] = records[0]; REQUIRE_FALSE(dk::AssetCatalog::create(files.root, records));
}

#ifdef _WIN32
TEST_CASE("catalog rejects Windows ordinal path aliases including Unicode case", "[assets]")
{
    AssetTestMemory memory; SceneTestFiles files;
    const dk::AssetCatalogRecord records[]{{*dk::AssetId::generate(), dk::AssetKind::mesh, "assets/Ä.gltf"},
        {*dk::AssetId::generate(), dk::AssetKind::texture, "assets/ä.gltf"}};
    REQUIRE(dk::AssetCatalog::create(files.root, records).error().code == dk::ErrorCode::conflict);
    write_test_source(files, "assets/Ä.gltf");
    auto catalog = dk::AssetCatalog::create(files.root, {records, 1}).value();
    REQUIRE(catalog.prepare_registration(catalog.guard(), first_registration("assets/ä.gltf")).error().code == dk::ErrorCode::conflict);
}
#endif

TEST_CASE("catalog allocation failure preserves the original snapshot and source files", "[assets]")
{
    AssetTestMemory memory; SceneTestFiles files; write_test_source(files);
    auto catalog = dk::AssetCatalog::create(files.root).value(); const auto guard = catalog.guard();
    auto tiny = memory.system.create_heap({"tiny", dk::memory::DomainCategory::assets, 1}).value();
    {
        dk::memory::DomainScope scope{tiny};
        REQUIRE_THROWS_AS(catalog.prepare_registration(guard, first_registration()), std::bad_alloc);
    }
    REQUIRE(tiny.snapshot().live_allocations == 0); REQUIRE(catalog.guard() == guard); REQUIRE(catalog.records().empty());
    REQUIRE_FALSE(std::filesystem::exists(files.root / *dk::path_from_utf8("assets/模型.gltf.meta")));
}

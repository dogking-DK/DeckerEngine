#include "AssetTestSupport.hpp"
#include "AssetPersistenceInternal.hpp"
#include "ContentDigest.hpp"
#include <nlohmann/json.hpp>
#include <catch2/generators/catch_generators.hpp>
#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#endif

namespace {
using namespace dk;
using namespace dk::asset_detail;
constexpr std::string_view source = "assets/模型.gltf", target = "assets/新名称.gltf";
constexpr std::string_view journal = ".decker/asset-operations/pending.json";
std::filesystem::path path(const SceneTestFiles& files, std::string_view name) { return files.root / *path_from_utf8(name); }
std::string text(const SceneTestFiles& files, std::string_view name)
{
    const auto bytes = read_file_bytes(path(files, name)); REQUIRE(bytes);
    return {reinterpret_cast<const char*>(bytes->data()), bytes->size()};
}
Result<void> fail() { return std::unexpected(Error{ErrorCode::io_error, "injected file failure"}); }
struct Fixture {
    AssetTestMemory memory;
    SceneTestFiles files;
    AssetCatalog catalog = AssetCatalog::create(files.root).value();
    Fixture() { write_test_source(files); files.write("project.json", "before"); }
    Result<void> registration() {
        auto candidate = catalog.prepare_registration(catalog.guard(), first_registration());
        REQUIRE(candidate); return catalog.commit_registration(std::move(*candidate), {"project.json", "before", "after"});
    }
    void registered() { REQUIRE(registration()); }
    Result<void> rename() {
        auto candidate = catalog.prepare_rename(catalog.guard(), source, target);
        REQUIRE(candidate); return catalog.commit_rename(std::move(*candidate), {"project.json", "after", "renamed"});
    }
};
void leave_pending(Fixture& f, bool rename)
{
    OperationHook hook = [](OperationStep step) -> Result<void> {
        if (step == OperationStep::finish || step == OperationStep::rollback_manifest) { return fail(); } return {};
    };
    ScopedOperationHook scope{hook};
    REQUIRE_FALSE((rename ? f.rename() : f.registration())); REQUIRE(f.catalog.needs_recovery());
    REQUIRE(std::filesystem::exists(path(f.files, journal)));
}
}

TEST_CASE("asset commit rolls back every file step without publishing", "[assets][persistence]")
{
    const auto failure = GENERATE(OperationStep::record, OperationStep::source, OperationStep::meta, OperationStep::manifest, OperationStep::finish);
    const bool rename = GENERATE(false, true);
    Fixture f; if (rename) { f.registered(); }
    const auto guard = f.catalog.guard(); const auto before = text(f.files, "project.json");
    const auto old_source = text(f.files, source);
    const auto old_meta = rename ? text(f.files, std::string{source} + ".meta") : std::string{};
    OperationHook hook = [&](OperationStep step) { return step == failure ? fail() : Result<void>{}; };
    ScopedOperationHook scope{hook}; const auto result = rename ? f.rename() : f.registration();
    REQUIRE_FALSE(result); REQUIRE(result.error().code == ErrorCode::io_error);
    REQUIRE(f.catalog.guard() == guard); REQUIRE_FALSE(f.catalog.needs_recovery());
    REQUIRE(text(f.files, "project.json") == before); REQUIRE(text(f.files, source) == old_source);
    REQUIRE_FALSE(std::filesystem::exists(path(f.files, target))); REQUIRE(check_asset_operations(f.files.root));
    if (rename) { REQUIRE(text(f.files, std::string{source} + ".meta") == old_meta); }
    else { REQUIRE_FALSE(std::filesystem::exists(path(f.files, std::string{source} + ".meta"))); REQUIRE(f.catalog.records().empty()); }
}
TEST_CASE("asset rollback failures retain diagnostics and support idempotent recovery", "[assets][persistence]")
{
    const auto failure = GENERATE(OperationStep::rollback_manifest, OperationStep::rollback_meta, OperationStep::rollback_source, OperationStep::rollback_finish);
    const bool rename = GENERATE(false, true);
    Fixture f; if (rename) { f.registered(); }
    const auto guard = f.catalog.guard(); const auto before = text(f.files, "project.json");
    {
        OperationHook hook = [&](OperationStep step) { return step == OperationStep::finish || step == failure ? fail() : Result<void>{}; };
        ScopedOperationHook scope{hook}; const auto result = rename ? f.rename() : f.registration();
        REQUIRE_FALSE(result); REQUIRE(result.error().message == "injected file failure");
        REQUIRE(std::any_of(result.error().context.begin(), result.error().context.end(), [](const auto& s) { return s.starts_with("rollback failed:"); }));
        REQUIRE(std::any_of(result.error().context.begin(), result.error().context.end(), [](const auto& s) { return s.starts_with("recovery record:"); }));
    }
    REQUIRE(f.catalog.guard() == guard); REQUIRE(f.catalog.needs_recovery()); REQUIRE_FALSE(check_asset_operations(f.files.root));
    REQUIRE(recover_asset_operations(f.files.root)); REQUIRE(recover_asset_operations(f.files.root));
    REQUIRE(text(f.files, "project.json") == before); REQUIRE(std::filesystem::exists(path(f.files, source)));
    REQUIRE_FALSE(std::filesystem::exists(path(f.files, target)));
    REQUIRE(f.catalog.needs_recovery()); // Recovery does not republish an old in-memory session.
    REQUIRE_FALSE(f.catalog.prepare_rename(f.catalog.guard(), source, target));
}
TEST_CASE("asset recovery preserves external changes and unknown records", "[assets][persistence]")
{
    const bool rename = GENERATE(false, true);
    Fixture f; if (rename) { f.registered(); } leave_pending(f, rename);
    const auto saved = text(f.files, journal), after = text(f.files, "project.json");
    const auto affected = std::string{rename ? target : source};
    const auto source_bytes = text(f.files, affected);
    f.files.write(*path_from_utf8(affected), "external edit");
    const auto result = recover_asset_operations(f.files.root); REQUIRE_FALSE(result); REQUIRE(result.error().code == ErrorCode::conflict);
    REQUIRE(text(f.files, journal) == saved); REQUIRE(text(f.files, "project.json") == after);
    REQUIRE(text(f.files, affected) == "external edit");
    f.files.write(*path_from_utf8(affected), source_bytes);
    f.files.write(".decker/asset-operations/unknown.tmp", "preserve");
    REQUIRE_FALSE(recover_asset_operations(f.files.root)); REQUIRE(text(f.files, ".decker/asset-operations/unknown.tmp") == "preserve");
    REQUIRE(std::filesystem::remove(path(f.files, ".decker/asset-operations/unknown.tmp")));
    REQUIRE(recover_asset_operations(f.files.root));
}
TEST_CASE("asset recovery rejects corrupt or unsupported journal before touching files", "[assets][persistence]")
{
    const auto mutation = GENERATE(0, 1, 2, 3, 4, 5, 6, 7, 8);
    Fixture f; leave_pending(f, false); const auto original = text(f.files, journal);
    auto json = nlohmann::json::parse(original);
    switch (mutation) {
    case 0: json["algorithm"] = "future-hash"; break;
    case 1: json["version"] = 2; break;
    case 2: json["meta"]["after"]["digest"] = std::string(32, '0'); break;
    case 3: json["source"] = "../outside.gltf"; break;
    case 4: json["unexpected"] = true; break;
    case 5: json["manifest"]["path"] = source; break;
    case 6: json["source_digest"] = std::string(32, 'A'); break;
    default: break;
    }
    auto changed = json.dump();
    if (mutation == 7) { changed.insert(1, "\"version\":1,"); }
    if (mutation == 8) { changed = "{"; }
    f.files.write(*path_from_utf8(journal), changed);
    const auto result = recover_asset_operations(f.files.root); REQUIRE_FALSE(result);
    if (mutation < 2) { REQUIRE(result.error().code == ErrorCode::not_supported); }
    REQUIRE(text(f.files, journal) == changed); REQUIRE(text(f.files, "project.json") == "after");
    f.files.write(*path_from_utf8(journal), original); REQUIRE(recover_asset_operations(f.files.root));
}
TEST_CASE("asset commit rejects stale candidates manifest and rename conflicts", "[assets][persistence]")
{
    Fixture f;
    auto candidate = f.catalog.prepare_registration(f.catalog.guard(), first_registration()).value();
    f.files.write("project.json", "external");
    REQUIRE_FALSE(f.catalog.commit_registration(candidate, {"project.json", "before", "after"}));
    REQUIRE(f.catalog.guard().revision == 0); REQUIRE_FALSE(std::filesystem::exists(path(f.files, journal)));
    f.files.write("project.json", "before"); f.registered();
    REQUIRE_FALSE(f.catalog.commit_registration(candidate, {"project.json", "after", "after"}));
    auto no_op = f.catalog.prepare_registration(f.catalog.guard(), {source}).value();
    const auto source_bytes = text(f.files, source);
    REQUIRE_FALSE(f.catalog.commit_registration(no_op, {source, source_bytes, source_bytes}));
    REQUIRE_FALSE(f.catalog.prepare_rename(f.catalog.guard(), source, "assets/模型.GLTF"));
    REQUIRE_FALSE(f.catalog.prepare_rename(f.catalog.guard(), source, "other/new.gltf"));
    REQUIRE_FALSE(f.catalog.prepare_rename(f.catalog.guard(), source, "assets/new.glb"));
    f.files.write(*path_from_utf8(target), "target owner");
    REQUIRE_FALSE(f.catalog.prepare_rename(f.catalog.guard(), source, target)); REQUIRE(text(f.files, target) == "target owner");
    REQUIRE(std::filesystem::remove(path(f.files, target)));
    auto renamed = f.catalog.prepare_rename(f.catalog.guard(), source, target).value();
    f.files.write(*path_from_utf8(std::string{target} + ".meta"), "target meta owner");
    REQUIRE_FALSE(f.catalog.commit_rename(std::move(renamed), {"project.json", "after", "renamed"}));
    REQUIRE(text(f.files, std::string{target} + ".meta") == "target meta owner"); REQUIRE(check_asset_operations(f.files.root));
}
TEST_CASE("asset exception after journal closes write gate and is recoverable", "[assets][persistence]")
{
    Fixture f;
    {
        OperationHook hook = [](OperationStep step) -> Result<void> { if (step == OperationStep::manifest) { throw std::bad_alloc{}; } return {}; };
        ScopedOperationHook scope{hook}; REQUIRE_THROWS_AS(f.registration(), std::bad_alloc);
    }
    REQUIRE(f.catalog.needs_recovery()); REQUIRE(f.catalog.guard().revision == 0); REQUIRE(recover_asset_operations(f.files.root));
    REQUIRE(text(f.files, "project.json") == "before"); REQUIRE_FALSE(std::filesystem::exists(path(f.files, std::string{source} + ".meta")));
}
TEST_CASE("asset changed registration restores exact prior sidecar and rejects other asset ownership", "[assets][persistence]")
{
    Fixture f; f.registered();
    const auto sidecar = std::string{source} + ".meta";
    const auto old_meta = text(f.files, sidecar) + " \n"; f.files.write(*path_from_utf8(sidecar), old_meta);
    auto candidate = f.catalog.prepare_registration(f.catalog.guard(), {source, {}, 3.0}).value();
    {
        OperationHook hook = [](OperationStep step) { return step == OperationStep::finish ? fail() : Result<void>{}; };
        ScopedOperationHook scope{hook};
        REQUIRE_FALSE(f.catalog.commit_registration(std::move(candidate), {"project.json", "after", "changed"}));
    }
    REQUIRE(text(f.files, sidecar) == old_meta); REQUIRE(f.catalog.inspect_source(source)->unit_scale == 1);
    REQUIRE(f.catalog.guard().revision == 1); REQUIRE(check_asset_operations(f.files.root));
    Vector<AssetCatalogRecord> records{f.catalog.records().begin(), f.catalog.records().end()};
    records.push_back({*AssetId::generate(), AssetKind::texture, String{sidecar.begin(), sidecar.end()}});
    auto aliased = AssetCatalog::create(f.files.root, records).value();
    auto updated = aliased.prepare_registration(aliased.guard(), {source, {}, 3.0}).value();
    REQUIRE_FALSE(aliased.commit_registration(std::move(updated), {"project.json", "after", "changed"}));
    REQUIRE_FALSE(aliased.prepare_rename(aliased.guard(), source, target)); REQUIRE(text(f.files, sidecar) == old_meta);
}
TEST_CASE("asset persistence rejects path aliases and real sharing failures preserve originals", "[assets][persistence]")
{
    Fixture f;
    REQUIRE_FALSE(f.catalog.prepare_rename(f.catalog.guard(), source, "assets/NUL.gltf"));
    auto candidate = f.catalog.prepare_registration(f.catalog.guard(), first_registration()).value();
    REQUIRE_FALSE(f.catalog.commit_registration(candidate, {"project.json.", "before", "after"}));
    std::filesystem::create_hard_link(path(f.files, source), path(f.files, "alias.gltf"));
    REQUIRE_FALSE(f.catalog.commit_registration(candidate, {"project.json", "before", "after"}));
    REQUIRE(std::filesystem::remove(path(f.files, "alias.gltf")));
#ifdef _WIN32
    const auto handle = CreateFileW(path(f.files, "project.json").c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    REQUIRE(handle != INVALID_HANDLE_VALUE);
    const auto result = f.catalog.commit_registration(candidate, {"project.json", "before", "after"});
    REQUIRE(CloseHandle(handle)); REQUIRE_FALSE(result); REQUIRE_FALSE(f.catalog.needs_recovery());
    REQUIRE(text(f.files, "project.json") == "before"); REQUIRE(check_asset_operations(f.files.root));
#endif
}

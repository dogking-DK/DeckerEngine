#include "AssetTestSupport.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <limits>

TEST_CASE("metadata codec roundtrips stable IDs settings and canonical output order", "[assets]")
{
    AssetTestMemory memory;
    auto meta = test_meta(); meta.unit_scale = 0.01;
    meta.outputs.push_back({"material/9999", *dk::AssetId::generate(), dk::AssetKind::material});
    meta.outputs.push_back({"texture/2", *dk::AssetId::generate(), dk::AssetKind::texture});
    const auto text = dk::serialize_asset_meta(meta); REQUIRE(text);
    const auto parsed = dk::parse_asset_meta(*text); REQUIRE(parsed);
    REQUIRE(parsed->root_id == meta.root_id); REQUIRE(parsed->unit_scale == 0.01);
    REQUIRE(*dk::serialize_asset_meta(*parsed) == *text);
    std::reverse(meta.outputs.begin(), meta.outputs.end());
    REQUIRE(*dk::serialize_asset_meta(meta) == *text);
    auto json = nlohmann::json::parse(text->begin(), text->end()); json["settings"] = nlohmann::json::object();
    const auto defaults = dk::parse_asset_meta(json.dump()); REQUIRE(defaults); REQUIRE(defaults->unit_scale == 1);
}

TEST_CASE("metadata codec rejects unknown schemas types versions and importer contracts", "[assets]")
{
    AssetTestMemory memory;
    const auto text = *dk::serialize_asset_meta(test_meta());
    const auto base = nlohmann::json::parse(text.begin(), text.end());
    for (const auto value : {nlohmann::json(true), nlohmann::json(1.0), nlohmann::json("1")}) {
        auto bad = base; bad["version"] = value; REQUIRE_FALSE(dk::parse_asset_meta(bad.dump()));
    }
    for (auto field : {"format", "version", "root_id", "importer", "settings", "outputs"}) {
        auto bad = base; bad.erase(field); REQUIRE_FALSE(dk::parse_asset_meta(bad.dump()));
    }
    auto bad = base; bad["version"] = 2; REQUIRE(dk::parse_asset_meta(bad.dump()).error().code == dk::ErrorCode::not_supported);
    bad = base; bad["importer"]["version"] = 2; REQUIRE(dk::parse_asset_meta(bad.dump()).error().code == dk::ErrorCode::not_supported);
    bad = base; bad["importer"]["name"] = "unknown"; REQUIRE(dk::parse_asset_meta(bad.dump()).error().code == dk::ErrorCode::not_supported);
    bad = base; bad["extra"] = 0; REQUIRE_FALSE(dk::parse_asset_meta(bad.dump()));
    bad = base; bad["settings"]["extra"] = 0; REQUIRE_FALSE(dk::parse_asset_meta(bad.dump()));
    bad = base; bad["settings"]["unit_scale"] = true; REQUIRE_FALSE(dk::parse_asset_meta(bad.dump()));
    bad = base; bad["outputs"][0]["kind"] = "unknown"; REQUIRE_FALSE(dk::parse_asset_meta(bad.dump()));
}

TEST_CASE("metadata codec rejects duplicate nested keys malformed UTF8 trailing data and budgets", "[assets]")
{
    AssetTestMemory memory;
    const auto encoded = *dk::serialize_asset_meta(test_meta());
    const std::string text{encoded};
    REQUIRE_FALSE(dk::parse_asset_meta(text + "{}"));
    REQUIRE_FALSE(dk::parse_asset_meta("// comment\n" + text));
    auto duplicate = text; duplicate.insert(duplicate.find("\"version\""), "\"version\":1,");
    REQUIRE_FALSE(dk::parse_asset_meta(duplicate));
    duplicate = text; duplicate.insert(duplicate.find("\"unit_scale\""), "\"unit_scale\":1,");
    REQUIRE_FALSE(dk::parse_asset_meta(duplicate));
    auto invalid = text; invalid.insert(invalid.find("mesh/0") + 2, 1, '\xff');
    REQUIRE_FALSE(dk::parse_asset_meta(invalid));
    REQUIRE_FALSE(dk::parse_asset_meta(std::string(dk::asset_meta_byte_limit + 1, ' ')));
    REQUIRE_FALSE(dk::parse_asset_meta(std::string(20, '[') + "0" + std::string(20, ']')));
    REQUIRE_FALSE(dk::parse_asset_meta(""));
}

TEST_CASE("metadata codec enforces root identity unique IDs canonical selectors and finite scale", "[assets]")
{
    AssetTestMemory memory;
    for (const auto scale : {0.0, -1.0, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) {
        auto meta = test_meta(); meta.unit_scale = scale; REQUIRE_FALSE(dk::serialize_asset_meta(meta));
    }
    for (const auto key : {"mesh/1", "material/00", "texture/-1", "material/10000", "texture/+1", "texture/1x", "texture/", "Mesh/0"}) {
        auto meta = test_meta(); meta.outputs.push_back({key, *dk::AssetId::generate(), dk::AssetKind::texture});
        REQUIRE_FALSE(dk::serialize_asset_meta(meta));
    }
    auto meta = test_meta(); meta.root_id = {}; REQUIRE_FALSE(dk::serialize_asset_meta(meta));
    meta = test_meta(); meta.root_id = *dk::AssetId::generate(); REQUIRE_FALSE(dk::serialize_asset_meta(meta));
    meta = test_meta(); meta.outputs.front().kind = dk::AssetKind::material; REQUIRE_FALSE(dk::serialize_asset_meta(meta));
    meta = test_meta(); meta.outputs.push_back({"texture/0", meta.root_id, dk::AssetKind::texture}); REQUIRE_FALSE(dk::serialize_asset_meta(meta));
    meta = test_meta(); meta.outputs.push_back({"mesh/0", *dk::AssetId::generate(), dk::AssetKind::mesh}); REQUIRE_FALSE(dk::serialize_asset_meta(meta));
    meta = test_meta(); meta.outputs.clear(); REQUIRE_FALSE(dk::serialize_asset_meta(meta));
    auto json = nlohmann::json::parse(dk::serialize_asset_meta(test_meta())->c_str());
    json["outputs"][0]["id"] = "00000000-0000-0000-0000-000000000000"; REQUIRE_FALSE(dk::parse_asset_meta(json.dump()));
}

TEST_CASE("metadata output count accepts the limit and rejects one extra", "[assets]")
{
    AssetTestMemory memory;
    auto meta = test_meta();
    for (std::size_t i = 0; i < dk::asset_catalog_record_limit - 1; ++i) {
        const auto key = "material/" + std::to_string(i);
        meta.outputs.push_back({dk::String{key.begin(), key.end()}, *dk::AssetId::generate(), dk::AssetKind::material});
    }
    const auto text = dk::serialize_asset_meta(meta); REQUIRE(text); REQUIRE(dk::parse_asset_meta(*text)->outputs.size() == 10000);
    meta.outputs.push_back({"texture/0", *dk::AssetId::generate(), dk::AssetKind::texture});
    REQUIRE_FALSE(dk::serialize_asset_meta(meta));
    auto json = nlohmann::json::parse(text->begin(), text->end()); json["outputs"].push_back(json["outputs"][0]);
    REQUIRE_FALSE(dk::parse_asset_meta(json.dump()));
}

TEST_CASE("metadata values retain their resource after the execution scope ends", "[assets]")
{
    auto system = dk::memory::MemorySystem::create().value();
    auto heap = system.create_heap({"assets", dk::memory::DomainCategory::assets}).value();
    std::optional<dk::AssetMetadata> escaped;
    {
        dk::memory::ThreadContext context{system}; dk::memory::ExecutionScope scope{context, heap};
        escaped = test_meta(); REQUIRE(escaped->outputs.get_allocator().resource() == heap);
    }
    REQUIRE(escaped->outputs[0].id == escaped->root_id);
    REQUIRE_FALSE(system.try_close().closed()); escaped.reset(); REQUIRE(system.try_close().closed());
}

#include <dk/assets/Metadata.hpp>
#include "AssetInternal.hpp"
#include <algorithm>

namespace dk {
namespace {
using namespace asset_detail;
using Json = nlohmann::json;
void fields(const Json& value, std::initializer_list<std::string_view> names)
{
    require(value.is_object() && value.size() == names.size(), "Unexpected meta object fields");
    for (const auto name : names) { require(value.contains(name), "Missing meta field: " + std::string{name}); }
}
void version(const Json& value)
{
    require(value.is_number_integer(), "Version must be an integer");
    require(value == 1, "Unsupported meta/importer version", ErrorCode::not_supported);
}
std::string string(const Json& value)
{ require(value.is_string(), "Expected a string"); return value.get<std::string>(); }
}
Result<AssetMetadata> parse_asset_meta(std::string_view text)
{
    return attempt<AssetMetadata>("parse_asset_meta", [&] {
        require(text.size() <= asset_meta_byte_limit, "Meta exceeds 2 MiB limit");
        std::vector<std::unordered_set<std::string>> keys;
        const auto json = Json::parse(text.begin(), text.end(), [&](int depth, Json::parse_event_t event, Json& value) {
            require(depth <= 16, "Meta nesting exceeds 16 levels");
            if (event == Json::parse_event_t::object_start) { keys.emplace_back(); }
            if (event == Json::parse_event_t::key) { require(keys.back().insert(string(value)).second, "Duplicate JSON object key"); }
            if (event == Json::parse_event_t::object_end) { keys.pop_back(); }
            return true;
        });
        fields(json, {"format", "version", "root_id", "importer", "settings", "outputs"});
        require(json.at("format") == "DeckerAssetMeta", "Unknown meta format"); version(json.at("version"));
        const auto& importer = json.at("importer"); fields(importer, {"name", "version"});
        require(string(importer.at("name")) == "gltf-static", "Unsupported importer", ErrorCode::not_supported);
        version(importer.at("version"));
        AssetMetadata meta; meta.root_id = take(AssetId::parse(string(json.at("root_id"))));
        const auto& settings = json.at("settings");
        require(settings.is_object() && (settings.empty() || (settings.size() == 1 && settings.contains("unit_scale"))), "Unexpected settings fields");
        if (!settings.empty()) {
            require(settings.at("unit_scale").is_number(), "unit_scale must be numeric");
            meta.unit_scale = settings.at("unit_scale").get<double>();
        }
        const auto& outputs = json.at("outputs");
        require(outputs.is_array() && outputs.size() <= asset_catalog_record_limit, "Invalid outputs array");
        meta.outputs.reserve(outputs.size());
        for (const auto& output : outputs) {
            fields(output, {"key", "id", "kind"});
            meta.outputs.push_back({owned(string(output.at("key"))), take(AssetId::parse(string(output.at("id")))),
                take(parse_asset_kind(string(output.at("kind"))))});
        }
        validate_meta(meta); return meta;
    });
}
Result<String> serialize_asset_meta(const AssetMetadata& meta)
{
    return attempt<String>("serialize_asset_meta", [&] {
        validate_meta(meta);
        std::vector<const AssetOutput*> sorted;
        for (const auto& output : meta.outputs) { sorted.push_back(&output); }
        std::sort(sorted.begin(), sorted.end(), [](auto a, auto b) { return a->key < b->key; });
        auto outputs = Json::array();
        for (const auto* output : sorted) {
            outputs.push_back({{"key", std::string{output->key}}, {"id", output->id.to_string()}, {"kind", asset_kind_name(output->kind)}});
        }
        const Json json{{"format", "DeckerAssetMeta"}, {"version", 1}, {"root_id", meta.root_id.to_string()},
            {"importer", {{"name", "gltf-static"}, {"version", 1}}}, {"settings", {{"unit_scale", meta.unit_scale}}}, {"outputs", std::move(outputs)}};
        const auto text = json.dump(2) + '\n'; require(text.size() <= asset_meta_byte_limit, "Encoded meta exceeds 2 MiB limit");
        return owned(text);
    });
}
} // namespace dk

#pragma once
#include <dk/assets/Metadata.hpp>
#include <dk/io/Path.hpp>
#include <nlohmann/json.hpp>
#include <charconv>
#include <cmath>
#include <unordered_set>

namespace dk::asset_detail {
struct Failure { Error error; };
inline void require(bool condition, std::string message, ErrorCode code = ErrorCode::invalid_argument)
{ if (!condition) { throw Failure{Error{code, std::move(message)}}; } }
template<class T> T take(Result<T> result)
{ if (!result) { throw Failure{std::move(result.error())}; } return std::move(*result); }
template<class T, class F> Result<T> attempt(std::string operation, F&& function)
{
    try { return function(); }
    catch (const Failure& failure) { return std::unexpected(failure.error.with_context(std::move(operation))); }
    catch (const nlohmann::json::exception& failure) {
        return std::unexpected(Error{ErrorCode::invalid_argument, failure.what(), {std::move(operation)}});
    }
}
inline String owned(std::string_view value) { return String{value.begin(), value.end()}; }
inline AssetKind selector_kind(std::string_view key)
{
    if (key == "mesh/0") { return AssetKind::mesh; }
    const auto slash = key.find('/');
    require(slash != key.npos && key.size() <= 13, "Invalid output selector: " + std::string{key});
    const auto kind = key.substr(0, slash), number = key.substr(slash + 1);
    require(kind == "material" || kind == "texture", "Unsupported output selector: " + std::string{key});
    require(!number.empty() && (number.size() == 1 || number.front() != '0'), "Noncanonical selector index");
    unsigned value = 0;
    const auto parsed = std::from_chars(number.data(), number.data() + number.size(), value);
    require(parsed.ec == std::errc{} && parsed.ptr == number.data() + number.size() && value < 10000, "Invalid selector index");
    return kind == "material" ? AssetKind::material : AssetKind::texture;
}
inline void validate_meta(const AssetMetadata& meta)
{
    require(!meta.root_id.is_nil(), "Nil root AssetId");
    require(std::isfinite(meta.unit_scale) && meta.unit_scale > 0, "unit_scale must be finite and positive");
    require(!meta.outputs.empty() && meta.outputs.size() <= asset_catalog_record_limit, "Invalid output count");
    std::unordered_set<std::string_view> keys;
    std::unordered_set<AssetId> ids;
    bool root = false;
    for (const auto& output : meta.outputs) {
        require(selector_kind(output.key) == output.kind, "Output kind differs from selector: " + std::string{output.key});
        require(keys.insert(output.key).second && !output.id.is_nil() && ids.insert(output.id).second, "Duplicate selector or invalid/duplicate AssetId");
        if (output.key == "mesh/0") { root = true; require(output.id == meta.root_id, "root_id must identify mesh/0"); }
    }
    require(root, "Missing mesh/0 output");
}
inline std::filesystem::path relative_path(std::string_view text)
{
    require(!text.empty() && text.size() <= 4096 && text.find_first_of("\\:") == text.npos
        && text.find("//") == text.npos && text.back() != '/', "Expected a normalized portable relative path");
    auto path = take(path_from_utf8(text));
    require(!path.has_root_path(), "Expected a relative path");
    for (const auto& part : path) { require(part != "." && part != ".." && !part.empty(), "Path escapes or is not normalized"); }
    return path;
}
} // namespace dk::asset_detail

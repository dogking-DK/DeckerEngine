#include <dk/assets/Catalog.hpp>
#include <dk/io/File.hpp>
#include "AssetInternal.hpp"
#include <algorithm>
#include <limits>
#include <map>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace dk {
namespace {
using namespace asset_detail;
struct PathLess {
    bool operator()(const std::filesystem::path& a, const std::filesystem::path& b) const
    {
#ifdef _WIN32
        return CompareStringOrdinal(a.c_str(), static_cast<int>(a.native().size()), b.c_str(), static_cast<int>(b.native().size()), TRUE) == CSTR_LESS_THAN;
#else
        return a.native() < b.native();
#endif
    }
};
bool same_path(const std::filesystem::path& a, const std::filesystem::path& b)
{ return !PathLess{}(a, b) && !PathLess{}(b, a); }
void validate_records(std::span<const AssetCatalogRecord> records)
{
    require(records.size() <= asset_catalog_record_limit, "Catalog exceeds 10000 records");
    std::unordered_set<AssetId> ids;
    std::map<std::filesystem::path, std::string_view, PathLess> paths;
    for (const auto& record : records) {
        require(!record.id.is_nil() && ids.insert(record.id).second, "Nil or duplicate catalog AssetId");
        require(!asset_kind_name(record.kind).empty(), "Unknown catalog asset kind");
        const auto path = relative_path(record.path);
        const auto [found, inserted] = paths.emplace(path, record.path);
        require(inserted || found->second == record.path, "Catalog has platform-equivalent path aliases", ErrorCode::conflict);
    }
}
bool regular_file(const std::filesystem::path& path)
{
    std::error_code error;
    const auto status = std::filesystem::status(path, error);
    if (error == std::errc::no_such_file_or_directory || (!error && !std::filesystem::exists(status))) { return false; }
    const auto display = take(path_to_utf8(path));
    require(!error, "Cannot inspect file: " + display + ": " + error.message(), ErrorCode::io_error);
    require(std::filesystem::is_regular_file(status), "Expected a regular file: " + display);
    return true;
}
std::optional<String> read_sidecar(const ProjectPaths& paths, std::string_view source)
{
    const auto relative = relative_path(source);
    require(source.size() <= 4091, "Source plus .meta exceeds path limit");
    const auto extension_path = relative.extension();
    require(!extension_path.empty(), "Only glTF/GLB registration is supported", ErrorCode::not_supported);
    auto extension = take(path_to_utf8(extension_path));
    for (auto& c : extension) { if (c >= 'A' && c <= 'Z') { c = static_cast<char>(c + ('a' - 'A')); } }
    require(extension == ".gltf" || extension == ".glb", "Only glTF/GLB registration is supported", ErrorCode::not_supported);
    const auto path = take(paths.resolve(relative));
    require(regular_file(path), "Source file not found: " + std::string{source}, ErrorCode::not_found);
    const auto meta_path = take(paths.resolve(relative_path(std::string{source} + ".meta")));
    if (!regular_file(meta_path)) { return {}; }
    const auto bytes = take(read_file_bytes(meta_path, asset_meta_byte_limit));
    if (bytes.empty()) { return owned(""); }
    return owned(std::string_view{reinterpret_cast<const char*>(bytes.data()), bytes.size()});
}
void check_records_against_meta(std::span<const AssetCatalogRecord> records, std::string_view source, const AssetMetadata& meta)
{
    const auto relative = relative_path(source);
    for (const auto& record : records) {
        const bool same = same_path(relative_path(record.path), relative);
        require(!same || record.path == source, "Source path spelling conflicts with catalog", ErrorCode::conflict);
        const auto output = std::find_if(meta.outputs.begin(), meta.outputs.end(), [&](const auto& item) { return item.id == record.id; });
        if (same) {
            require(output != meta.outputs.end() && output->kind == record.kind, "Meta disagrees with catalog record: " + record.id.to_string(), ErrorCode::conflict);
        } else {
            require(output == meta.outputs.end(), "AssetId already belongs to another source: " + record.id.to_string(), ErrorCode::conflict);
        }
    }
}
}
Result<AssetCatalog> AssetCatalog::create(const std::filesystem::path& root, std::span<const AssetCatalogRecord> records)
{
    return attempt<AssetCatalog>("AssetCatalog.create", [&] {
        validate_records(records);
        auto paths = take(ProjectPaths::create(root));
        Vector<AssetCatalogRecord> copy{records.begin(), records.end()};
        std::sort(copy.begin(), copy.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
        return AssetCatalog{std::move(paths), std::move(copy), {take(CatalogSessionId::generate()), 0}};
    });
}
Result<void> AssetCatalog::check_guard(CatalogGuard guard) const
{
    if (guard != guard_) { return std::unexpected(Error{ErrorCode::conflict, "Stale or foreign catalog guard", {"AssetCatalog.check_guard"}}); }
    return {};
}
Result<AssetMetadata> AssetCatalog::inspect_source(std::string_view source) const
{
    return attempt<AssetMetadata>("AssetCatalog.inspect_source: " + std::string{source}, [&] {
        const auto bytes = read_sidecar(paths_, source);
        require(bytes.has_value(), "Asset meta not found", ErrorCode::not_found);
        auto meta = take(parse_asset_meta(*bytes));
        check_records_against_meta(records_, source, meta);
        return meta;
    });
}
Result<RegistrationCandidate> AssetCatalog::prepare_registration(CatalogGuard guard, const RegistrationRequest& request) const
{
    return attempt<RegistrationCandidate>("AssetCatalog.prepare_registration: " + std::string{request.source}, [&] {
        const auto checked = check_guard(guard); if (!checked) { throw Failure{checked.error()}; }
        require(request.missing_meta == MissingMetaPolicy::reject || request.missing_meta == MissingMetaPolicy::create_or_adopt, "Unknown missing-meta policy");
        require(request.outputs.size() <= asset_catalog_record_limit, "Too many requested outputs");
        std::unordered_set<std::string_view> selectors;
        bool root = request.outputs.empty();
        for (const auto& output : request.outputs) {
            require(selector_kind(output.key) == output.kind && selectors.insert(output.key).second, "Invalid or duplicate requested selector");
            root = root || output.key == "mesh/0";
        }
        require(root, "Requested outputs must include mesh/0");
        auto original = read_sidecar(paths_, request.source);
        AssetMetadata meta;
        std::optional<String> canonical_before;
        if (original) {
            meta = take(parse_asset_meta(*original));
            canonical_before = take(serialize_asset_meta(meta));
            check_records_against_meta(records_, request.source, meta);
        } else {
            require(request.missing_meta == MissingMetaPolicy::create_or_adopt, "Asset meta not found; explicit creation/adoption required", ErrorCode::not_found);
            const auto path = relative_path(request.source);
            const AssetCatalogRecord* legacy = nullptr;
            for (const auto& record : records_) {
                if (!same_path(relative_path(record.path), path)) { continue; }
                require(record.path == request.source && !legacy && record.kind == AssetKind::mesh, "Ambiguous legacy identity or path alias", ErrorCode::conflict);
                legacy = &record;
            }
            meta.root_id = legacy ? legacy->id : take(AssetId::generate());
            meta.outputs.push_back({owned("mesh/0"), meta.root_id, AssetKind::mesh});
        }
        if (request.unit_scale) { meta.unit_scale = *request.unit_scale; }
        for (const auto& output : request.outputs) {
            const auto found = std::find_if(meta.outputs.begin(), meta.outputs.end(), [&](const auto& item) { return item.key == output.key; });
            if (found == meta.outputs.end()) { meta.outputs.push_back({owned(output.key), take(AssetId::generate()), output.kind}); }
        }
        const auto encoded = take(serialize_asset_meta(meta));
        check_records_against_meta(records_, request.source, meta);
        Vector<AssetCatalogRecord> records = records_;
        for (const auto& output : meta.outputs) {
            if (std::none_of(records.begin(), records.end(), [&](const auto& item) { return item.id == output.id; })) {
                require(records.size() < asset_catalog_record_limit, "Registration exceeds catalog record limit");
                records.push_back({output.id, output.kind, owned(request.source)});
            }
        }
        std::sort(records.begin(), records.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
        auto next = guard_;
        if (!canonical_before || *canonical_before != encoded || records != records_) {
            require(next.revision != std::numeric_limits<std::uint64_t>::max(), "Catalog revision exhausted", ErrorCode::invalid_state);
            ++next.revision;
        }
        return RegistrationCandidate{guard_, next, owned(request.source), std::move(meta), std::move(records), std::move(original)};
    });
}
Result<void> AssetCatalog::validate_registration(const RegistrationCandidate& candidate) const
{
    const auto result = attempt<bool>("AssetCatalog.validate_registration", [&] {
        const auto checked = check_guard(candidate.base_guard()); if (!checked) { throw Failure{checked.error()}; }
        require(read_sidecar(paths_, candidate.source()) == candidate.expected_meta_bytes(), "Sidecar changed after preparation", ErrorCode::conflict);
        return true;
    });
    if (!result) { return std::unexpected(result.error()); }
    return {};
}
} // namespace dk

#include "AssetPersistenceInternal.hpp"
#include "AssetInternal.hpp"
#include "ContentDigest.hpp"
#include <dk/io/File.hpp>
#include <dk/profiling/Profiler.hpp>
#include <algorithm>
#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#endif

namespace dk::asset_detail {
namespace {
namespace fs = std::filesystem;
using Json = nlohmann::json;
constexpr std::string_view journal_name = ".decker/asset-operations/pending.json";
constexpr std::size_t manifest_limit = 16 * 1024 * 1024, journal_limit = 128 * 1024 * 1024;
thread_local const OperationHook* current_hook = nullptr;
void step(OperationStep value) { if (current_hook) { take((*current_hook)(value)); } }
std::string digest(std::string_view text) { return content_digest(std::as_bytes(std::span{text.data(), text.size()})).hex(); }
void io(bool ok, std::string message) { require(ok, std::move(message), ErrorCode::io_error); }
bool file_exists(const fs::path& path)
{
    std::error_code error;
    const auto status = fs::symlink_status(path, error);
    if (error == std::errc::no_such_file_or_directory) { return false; }
    io(!error, "Cannot inspect " + take(path_to_utf8(path)) + ": " + error.message());
    return fs::exists(status);
}
std::string read(const fs::path& path, std::size_t limit)
{
    const auto bytes = take(read_file_bytes(path, limit));
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}
void write(const fs::path& path, std::string_view text)
{
    take(write_file_bytes_atomic(path, std::as_bytes(std::span{text.data(), text.size()}),
        [&](const fs::path& temporary) -> Result<void> {
            return attempt<void>("verify asset temporary", [&] {
                require(read(temporary, text.size()) == text, "Temporary file differs", ErrorCode::io_error);
            });
        }));
}
void erase_file(const fs::path& path)
{
    std::error_code error;
    const bool removed = fs::remove(path, error);
    io(!error && removed, "Cannot remove " + take(path_to_utf8(path)) + ": " + error.message());
}
void move(const fs::path& from, const fs::path& to)
{
#ifdef _WIN32
    if (!MoveFileExW(from.c_str(), to.c_str(), 0)) {
        const std::error_code error{static_cast<int>(GetLastError()), std::system_category()};
        io(false, "Cannot rename " + take(path_to_utf8(from)) + " to " + take(path_to_utf8(to)) + ": " + error.message());
    }
#else
    (void)from; (void)to;
    require(false, "Asset persistence requires Windows", ErrorCode::not_supported);
#endif
}
struct Image { std::string text, hash; bool operator==(const Image&) const = default; };
struct Change { std::string path; std::optional<Image> before; Image after; };
struct Record { std::string source, target, source_hash; Change meta, manifest; bool rename() const { return source != target; } };
Image image(std::string_view text) { return {std::string{text}, digest(text)}; }
Json encode_image(const std::optional<Image>& value)
{ return value ? Json{{"text", value->text}, {"digest", value->hash}} : Json(nullptr); }
Json encode_change(const Change& value)
{ return Json{{"path", value.path}, {"before", encode_image(value.before)}, {"after", encode_image(value.after)}}; }
std::string encode(const Record& value)
{
    const Json json{{"format", "DeckerAssetOperation"}, {"version", 1}, {"algorithm", "xxh3-128-v1"},
        {"kind", value.rename() ? "rename" : "registration"}, {"source", value.source}, {"target", value.target},
        {"source_digest", value.source_hash}, {"meta", encode_change(value.meta)}, {"manifest", encode_change(value.manifest)}};
    auto text = json.dump(2) + '\n'; require(text.size() <= journal_limit, "Operation record too large"); return text;
}
void fields(const Json& json, std::initializer_list<std::string_view> names)
{
    require(json.is_object() && json.size() == names.size(), "Invalid operation record fields");
    for (const auto name : names) { require(json.contains(name), "Missing operation field: " + std::string{name}); }
}
std::string string(const Json& value) { require(value.is_string(), "Expected operation string"); return value.get<std::string>(); }
std::string hash(const Json& value)
{
    auto text = string(value);
    require(text.size() == 32 && std::all_of(text.begin(), text.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }), "Invalid canonical digest");
    return text;
}
std::optional<Image> decode_image(const Json& json, std::size_t limit)
{
    if (json.is_null()) { return {}; }
    fields(json, {"text", "digest"});
    auto text = string(json.at("text")); require(text.size() <= limit, "Operation image exceeds limit");
    auto expected = hash(json.at("digest")); require(digest(text) == expected, "Corrupt operation image digest", ErrorCode::conflict);
    return Image{std::move(text), std::move(expected)};
}
Change decode_change(const Json& json, std::size_t limit)
{
    fields(json, {"path", "before", "after"});
    auto after = decode_image(json.at("after"), limit); require(after.has_value(), "Missing operation after image");
    return {string(json.at("path")), decode_image(json.at("before"), limit), std::move(*after)};
}
Record decode(std::string_view text)
{
    std::vector<std::unordered_set<std::string>> keys;
    const auto json = Json::parse(text.begin(), text.end(), [&](int depth, Json::parse_event_t event, Json& value) {
        require(depth <= 16, "Operation nesting exceeds 16 levels");
        if (event == Json::parse_event_t::object_start) { keys.emplace_back(); }
        if (event == Json::parse_event_t::key) { require(keys.back().insert(string(value)).second, "Duplicate operation key"); }
        if (event == Json::parse_event_t::object_end) { keys.pop_back(); }
        return true;
    });
    fields(json, {"format", "version", "algorithm", "kind", "source", "target", "source_digest", "meta", "manifest"});
    require(json.at("format") == "DeckerAssetOperation", "Unknown operation format");
    require(json.at("version").is_number_integer(), "Operation version must be integer");
    require(json.at("version") == 1 && json.at("algorithm") == "xxh3-128-v1", "Unsupported operation version or digest algorithm", ErrorCode::not_supported);
    Record result{string(json.at("source")), string(json.at("target")), hash(json.at("source_digest")),
        decode_change(json.at("meta"), asset_meta_byte_limit), decode_change(json.at("manifest"), manifest_limit)};
    require(json.at("kind") == (result.rename() ? "rename" : "registration"), "Operation kind disagrees with paths");
    require(result.manifest.before.has_value(), "Manifest must have before image");
    require(!result.rename() || result.meta.before == result.meta.after, "Rename must preserve meta bytes");
    return result;
}
fs::path data_path(const ProjectPaths& paths, std::string_view text)
{
    const auto relative = relative_path(text);
    require(!same_asset_path(*relative.begin(), fs::path{".decker"}), "Asset data cannot use .decker paths");
    return persistent_path(paths, text);
}
void validate_paths(const ProjectPaths& paths, const Record& record)
{
    require(record.source.size() <= 4091 && record.target.size() <= 4091, "Source plus .meta exceeds path limit");
    const auto source = data_path(paths, record.source), target = data_path(paths, record.target);
    auto extension = take(path_to_utf8(source.extension()));
    std::transform(extension.begin(), extension.end(), extension.begin(), [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c; });
    require(extension == ".gltf" || extension == ".glb", "Unsupported asset source extension", ErrorCode::not_supported);
    if (record.rename()) {
        require(!same_asset_path(source, target), "Equivalent rename paths", ErrorCode::conflict);
        require(source.parent_path() == target.parent_path() && source.extension() == target.extension(), "Rename requires same directory and extension");
    }
    require(record.meta.path == record.source + ".meta", "Sidecar path differs from source");
    const auto meta = data_path(paths, record.meta.path), new_meta = data_path(paths, record.target + ".meta");
    const auto manifest = data_path(paths, record.manifest.path);
    for (const auto& path : {source, target, meta, new_meta}) {
        require(!same_asset_path(path, manifest), "Manifest overlaps asset paths", ErrorCode::conflict);
    }
}
std::optional<std::string> actual(const fs::path& path)
{ if (!file_exists(path)) { return {}; } return take(file_digest(path)).hex(); }
bool matches(const std::optional<std::string>& current, const std::optional<Image>& expected)
{ return expected ? current && *current == expected->hash : !current; }
bool at_after(const ProjectPaths& paths, const Change& change)
{
    const auto current = actual(data_path(paths, change.path));
    const bool before = matches(current, change.before), after = matches(current, change.after);
    require(before || after, "Unrecognized file state: " + change.path, ErrorCode::conflict);
    return !before && after;
}
bool moved(const ProjectPaths& paths, std::string_view source, std::string_view target, const std::string& expected)
{
    const auto a = actual(data_path(paths, source)), b = actual(data_path(paths, target));
    require((a && *a == expected && !b) || (!a && b && *b == expected),
        "Ambiguous rename state: " + std::string{source} + " -> " + std::string{target}, ErrorCode::conflict);
    return b.has_value();
}
void restore(const ProjectPaths& paths, const Change& change)
{
    if (!at_after(paths, change)) { return; }
    const auto path = data_path(paths, change.path);
    if (change.before) { write(path, change.before->text); } else { erase_file(path); }
}
void rollback(const ProjectPaths& paths, const Record& record, std::string_view journal_text)
{
    require(read(persistent_path(paths, journal_name), journal_limit) == journal_text, "Operation record changed", ErrorCode::conflict);
    validate_paths(paths, record);
    // Preflight all files before any compensation, including the untouched registration source.
    (void)at_after(paths, record.manifest);
    if (record.rename()) {
        (void)moved(paths, record.source, record.target, record.source_hash);
        (void)moved(paths, record.meta.path, record.target + ".meta", record.meta.after.hash);
    } else {
        require(actual(data_path(paths, record.source)) == record.source_hash, "Source changed during operation", ErrorCode::conflict);
        (void)at_after(paths, record.meta);
    }
    step(OperationStep::rollback_manifest); restore(paths, record.manifest);
    step(OperationStep::rollback_meta);
    if (record.rename()) {
        if (moved(paths, record.meta.path, record.target + ".meta", record.meta.after.hash)) {
            move(data_path(paths, record.target + ".meta"), data_path(paths, record.meta.path));
        }
    } else { restore(paths, record.meta); }
    step(OperationStep::rollback_source);
    if (record.rename() && moved(paths, record.source, record.target, record.source_hash)) {
        move(data_path(paths, record.target), data_path(paths, record.source));
    }
    step(OperationStep::rollback_finish);
    require(read(persistent_path(paths, journal_name), journal_limit) == journal_text, "Operation record changed", ErrorCode::conflict);
    erase_file(persistent_path(paths, journal_name));
}
std::vector<fs::path> entries(const ProjectPaths& paths)
{
    const auto directory = persistent_path(paths, ".decker/asset-operations");
    if (!file_exists(directory)) { return {}; }
    std::error_code error;
    fs::directory_iterator iterator{directory, error}; io(!error, "Cannot inspect operation directory: " + error.message());
    std::vector<fs::path> result;
    for (const auto end = fs::directory_iterator{}; iterator != end; iterator.increment(error)) {
        io(!error, "Cannot enumerate operation directory: " + error.message()); result.push_back(iterator->path());
    }
    io(!error, "Cannot enumerate operation directory: " + error.message()); return result;
}
}
ScopedOperationHook::ScopedOperationHook(const OperationHook& hook) noexcept : previous_(current_hook) { current_hook = &hook; }
ScopedOperationHook::~ScopedOperationHook() { current_hook = previous_; }
void validate_asset_operation_paths(const ProjectPaths& paths, std::string_view source,
    std::string_view target, std::string_view manifest)
{
    validate_paths(paths, Record{std::string{source}, std::string{target}, {},
        {std::string{source} + ".meta", {}, {}}, {std::string{manifest}, {}, {}}});
}
bool same_asset_path(const fs::path& a, const fs::path& b)
{
#ifdef _WIN32
    return CompareStringOrdinal(a.c_str(), static_cast<int>(a.native().size()), b.c_str(), static_cast<int>(b.native().size()), TRUE) == CSTR_EQUAL;
#else
    return a == b;
#endif
}
fs::path persistent_path(const ProjectPaths& paths, std::string_view text)
{
    const auto relative = relative_path(text);
#ifdef _WIN32
    require(GetDriveTypeW(paths.root().root_path().c_str()) != DRIVE_REMOTE && paths.root().root_name().native().size() == 2,
        "Asset persistence requires a Windows local drive", ErrorCode::not_supported);
    auto current = paths.root();
    for (const auto& part : relative) {
        const auto name = part.native();
        require(name.back() != L'.' && name.back() != L' ' && name.find_first_of(L"<>:\"|?*") == name.npos
            && std::none_of(name.begin(), name.end(), [](wchar_t c) { return c < 32; }), "Invalid ordinary file name");
        auto base = name.substr(0, name.find(L'.'));
        while (!base.empty() && base.back() == L' ') { base.pop_back(); }
        std::transform(base.begin(), base.end(), base.begin(), [](wchar_t c) { return c >= L'a' && c <= L'z' ? static_cast<wchar_t>(c - (L'a' - L'A')) : c; });
        require(base != L"CON" && base != L"PRN" && base != L"AUX" && base != L"NUL" && base != L"CONIN$" && base != L"CONOUT$"
            && !(base.size() == 4 && (base.starts_with(L"COM") || base.starts_with(L"LPT"))
                && ((base[3] >= L'0' && base[3] <= L'9') || base[3] == L'\u00b9' || base[3] == L'\u00b2' || base[3] == L'\u00b3')),
            "Reserved Windows file name");
        current /= part;
        const auto attributes = GetFileAttributesW(current.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            const auto error = GetLastError();
            io(error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND, "Cannot inspect asset path: " + std::string{text});
            continue;
        }
        require((attributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DEVICE)) == 0, "Asset path has a reparse point or device");
        // Reject 8.3 spellings, which can otherwise alias a different recorded path.
        std::wstring long_name(32768, L'\0');
        const auto length = GetLongPathNameW(current.c_str(), long_name.data(), static_cast<DWORD>(long_name.size()));
        io(length > 0 && length < long_name.size(), "Cannot resolve ordinary asset path"); long_name.resize(length);
        require(same_asset_path(current, fs::path{long_name}), "Asset path uses a short-name alias", ErrorCode::conflict);
        if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
            std::error_code error; const auto links = fs::hard_link_count(current, error);
            io(!error, "Cannot inspect asset hard links"); require(links == 1, "Asset persistence rejects hard-linked files", ErrorCode::conflict);
        }
    }
    return current;
#else
    (void)paths; (void)relative;
    throw Failure{Error{ErrorCode::not_supported, "Asset persistence requires Windows"}};
#endif
}
Result<void> commit_asset_files(const ProjectPaths& paths, std::string_view source, std::string_view target,
    const std::optional<String>& before_meta, std::string_view after_meta, const AssetManifestUpdate& manifest, bool& needs_recovery)
{
    DK_PROFILE_ZONE("Assets.Commit");
    return attempt<void>("commit_asset_files", [&] {
        require(!needs_recovery, "Catalog requires recovery and reopen", ErrorCode::invalid_state);
        take(check_asset_operations(paths.root()));
        require(after_meta.size() <= asset_meta_byte_limit && manifest.before.size() <= manifest_limit && manifest.after.size() <= manifest_limit,
            "Operation image exceeds limit");
        Record record{std::string{source}, std::string{target}, {},
            {std::string{source} + ".meta", before_meta ? std::optional{image(*before_meta)} : std::nullopt, image(after_meta)},
            {std::string{manifest.path}, image(manifest.before), image(manifest.after)}};
        validate_paths(paths, record);
        record.source_hash = take(file_digest(data_path(paths, source))).hex();
        require(matches(actual(data_path(paths, record.meta.path)), record.meta.before), "Meta changed before commit", ErrorCode::conflict);
        require(matches(actual(data_path(paths, manifest.path)), record.manifest.before), "Manifest changed before commit", ErrorCode::conflict);
        if (record.rename()) {
            require(record.meta.before == record.meta.after, "Rename changes metadata");
            require(!file_exists(data_path(paths, target)) && !file_exists(data_path(paths, std::string{target} + ".meta")), "Rename target exists", ErrorCode::conflict);
        }
        const auto text = encode(record);
        const auto journal = persistent_path(paths, journal_name);
        std::error_code error; fs::create_directories(journal.parent_path(), error); io(!error, "Cannot create operation directory: " + error.message());
        (void)persistent_path(paths, journal_name);
        step(OperationStep::record);
        // Once a journal write starts, even an exception must close the write gate.
        needs_recovery = true;
        const auto saved = attempt<void>("write operation record", [&] { write(journal, text); });
        if (!saved) {
            needs_recovery = !check_asset_operations(paths.root()).has_value();
            throw Failure{saved.error()};
        }
        const auto result = attempt<void>("apply asset operation", [&] {
            step(OperationStep::source);
            if (record.rename()) { move(data_path(paths, source), data_path(paths, target)); }
            step(OperationStep::meta);
            if (record.rename()) { move(data_path(paths, record.meta.path), data_path(paths, std::string{target} + ".meta")); }
            else { write(data_path(paths, record.meta.path), record.meta.after.text); }
            step(OperationStep::manifest); write(data_path(paths, manifest.path), manifest.after);
            require(actual(data_path(paths, target)) == record.source_hash
                && matches(actual(data_path(paths, std::string{target} + ".meta")), record.meta.after)
                && matches(actual(data_path(paths, manifest.path)), record.manifest.after), "Final asset files differ", ErrorCode::conflict);
            step(OperationStep::finish);
            require(read(journal, journal_limit) == text, "Operation record changed", ErrorCode::conflict);
            erase_file(journal);
        });
        if (!result) {
            const auto restored = attempt<void>("rollback asset operation", [&] { rollback(paths, record, text); });
            auto failure = result.error();
            if (!restored) {
                failure.context.push_back("rollback failed: " + restored.error().message);
                failure.context.insert(failure.context.end(), restored.error().context.begin(), restored.error().context.end());
                failure.context.push_back("recovery record: " + take(path_to_utf8(journal)));
            } else { needs_recovery = false; }
            throw Failure{std::move(failure)};
        }
        needs_recovery = false;
    });
}
} // namespace dk::asset_detail

namespace dk {
Result<void> check_asset_operations(const std::filesystem::path& root)
{
    return asset_detail::attempt<void>("check_asset_operations", [&] {
        const auto paths = asset_detail::take(ProjectPaths::create(root));
        const auto pending = asset_detail::entries(paths);
        asset_detail::require(pending.empty(), "Unfinished or unknown asset operation; recover before writing: "
            + (pending.empty() ? std::string{} : asset_detail::take(path_to_utf8(pending.front()))), ErrorCode::invalid_state);
    });
}
Result<void> recover_asset_operations(const std::filesystem::path& root)
{
    DK_PROFILE_ZONE("Assets.Recover");
    return asset_detail::attempt<void>("recover_asset_operations", [&] {
        using namespace asset_detail;
        const auto paths = take(ProjectPaths::create(root));
        const auto pending = entries(paths);
        if (pending.empty()) { return; }
        const auto journal = persistent_path(paths, journal_name);
        require(pending.size() == 1 && pending.front() == journal, "Unknown operation files; preserve for manual recovery", ErrorCode::conflict);
        const auto result = attempt<void>(take(path_to_utf8(journal)), [&] {
            const auto text = read(journal, journal_limit); rollback(paths, decode(text), text);
        });
        take(result);
    });
}
} // namespace dk

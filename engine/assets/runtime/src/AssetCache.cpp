#include "AssetCacheInternal.hpp"
#include "CpuArtifactInternal.hpp"
#include <dk/assets/GltfImporter.hpp>
#include <dk/profiling/Profiler.hpp>
#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#endif

namespace dk::asset_detail {
namespace {
thread_local const CacheHook* current_hook = nullptr;
}
ScopedCacheHook::ScopedCacheHook(const CacheHook& hook) noexcept : previous_{current_hook} { current_hook = &hook; }
ScopedCacheHook::~ScopedCacheHook() { current_hook = previous_; }
void cache_step(CacheStep step) { if (current_hook) { take((*current_hook)(step)); } }
bool cache_path_exists(const std::filesystem::path& path)
{
    std::error_code error; const auto status = std::filesystem::symlink_status(path, error);
    if (error == std::errc::no_such_file_or_directory) { return false; }
    require(!error, "Cannot inspect cache path: " + error.message(), ErrorCode::io_error); return std::filesystem::exists(status);
}
std::string read_cache_text(const std::filesystem::path& path, std::size_t limit)
{
    const auto bytes = take(read_file_bytes(path, limit));
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}
std::optional<std::string> optional_cache_text(const std::filesystem::path& path, std::size_t limit)
{ return cache_path_exists(path) ? std::optional{read_cache_text(path, limit)} : std::nullopt; }
void write_cache_text(const std::filesystem::path& path, std::string_view text)
{
    take(write_file_bytes_atomic(path, std::as_bytes(std::span{text.data(), text.size()}), [&](const auto& temporary) {
        return attempt<void>("verify cache file", [&] {
            require(read_cache_text(temporary, text.size()) == text, "Cache write differs", ErrorCode::io_error);
        });
    }));
}
void ordinary_cache_input(const ProjectPaths& paths, std::string_view name)
{
    const auto relative = relative_path(name);
    require(!same_asset_path(*relative.begin(), std::filesystem::path{".decker"}), "Reserved cache input path");
    (void)persistent_path(paths, name);
}
void verify_cache_inputs(const ProjectPaths& paths, std::span<const InputFingerprint> inputs)
{
    DK_PROFILE_ZONE("Assets.CacheVerifyInputs");
    std::size_t total = 0;
    for (const auto& input : inputs) {
        ordinary_cache_input(paths, input.path);
        const auto limit = std::min<std::size_t>(64 * 1024 * 1024, 128 * 1024 * 1024 - total);
        auto bytes = take(read_file_bytes(persistent_path(paths, input.path), limit)); total += bytes.size();
        require(bytes.size() == input.bytes && content_digest(bytes).hex() == std::string_view{input.digest},
            "Cache input changed: " + std::string{input.path}, ErrorCode::conflict);
    }
}
}
namespace dk {
namespace {
namespace fs = std::filesystem;
using namespace asset_detail;
using Json = nlohmann::json;
void directory(const ProjectPaths& paths, std::string_view name)
{
    std::error_code error; fs::create_directories(persistent_path(paths, name), error);
    require(!error, "Cannot create cache directory: " + error.message(), ErrorCode::io_error);
    (void)persistent_path(paths, name);
}
void move(const fs::path& from, const fs::path& to)
{
#ifdef _WIN32
    if (!MoveFileExW(from.c_str(), to.c_str(), 0)) {
        const std::error_code error{static_cast<int>(GetLastError()), std::system_category()};
        require(false, "Cannot publish cache: " + error.message(), ErrorCode::io_error);
    }
#else
    (void)from; (void)to; require(false, "Cache publication requires Windows", ErrorCode::not_supported);
#endif
}
struct StagedEntry {
    const ProjectPaths& paths;
    std::string temporary, destination;
    std::array<std::pair<std::string, ContentDigest>, 3> files;
    bool active = false, published = false;
    std::string_view name() const noexcept { return published ? destination : temporary; }
    bool cleanup() noexcept {
        if (!active) { return true; } active = false;
        try {
            for (const auto& [file, expected] : files) {
                const auto path = persistent_path(paths, std::string{name()} + '/' + file);
                if (cache_path_exists(path) && take(file_digest(path)) != expected) { return false; }
            }
            for (const auto& [file, expected] : files) {
                (void)expected; std::error_code error; fs::remove(persistent_path(paths, std::string{name()} + '/' + file), error);
                if (error) { return false; }
            }
            std::error_code error; return fs::remove(persistent_path(paths, name()), error) && !error;
        } catch (...) { return false; }
    }
    ~StagedEntry() { (void)cleanup(); }
};
std::string current_name(AssetId root) { return std::string{cache_root} + "/current/" + root.to_string() + ".json"; }
ContentDigest hash_text(std::string_view text) { return content_digest(std::as_bytes(std::span{text.data(), text.size()})); }
void summarize(CachedAsset& result)
{
    Json outputs = Json::array(), inputs = Json::array(), diagnostics = Json::array();
    for (const auto& o : result.artifact.data.outputs) { outputs.push_back({{"key", std::string{o.key}}, {"id", o.id.to_string()}, {"kind", asset_kind_name(o.kind)}}); }
    for (const auto& i : result.artifact.inputs) { inputs.push_back({{"path", std::string{i.path}}, {"bytes", i.bytes}, {"digest", std::string{i.digest}}}); }
    for (const auto& d : result.artifact.data.diagnostics) { diagnostics.push_back(std::string{d}); }
    result.summary_json = owned(Json{{"format", "DeckerAssetCacheResult"}, {"version", 1},
        {"source", std::string{result.artifact.source}}, {"root_id", result.artifact.data.mesh.id.to_string()},
        {"key", std::string{result.key}}, {"directory", std::string{result.directory}}, {"cache_hit", result.cache_hit},
        {"miss_reason", std::string{result.miss_reason}}, {"unit_scale", result.artifact.data.unit_scale},
        {"outputs", std::move(outputs)}, {"inputs", std::move(inputs)}, {"diagnostics", std::move(diagnostics)}}.dump() + '\n');
}
}
Result<CachedAsset> compile_cached_asset(const ProjectPaths& paths, const AssetCacheRequest& request)
{
    DK_PROFILE_ZONE("Assets.CacheCompile");
    return attempt<CachedAsset>("compile_cached_asset", [&] {
        take(check_asset_operations(paths.root())); ordinary_cache_input(paths, request.source);
        const auto meta_name = std::string{request.source} + ".meta";
        const auto before_meta = optional_cache_text(persistent_path(paths, meta_name), asset_meta_byte_limit);
        std::optional<AssetMetadata> previous;
        if (before_meta) { previous = take(parse_asset_meta(*before_meta)); }
        const auto scale = request.unit_scale.value_or(previous ? previous->unit_scale : 1);
        require(std::isfinite(scale) && scale > 0, "unit_scale must be finite and positive");
        std::optional<std::string> before_index;
        std::string miss = "no metadata";
        if (previous) {
            before_index = optional_cache_text(persistent_path(paths, current_name(previous->root_id)), cache_index_limit);
            miss = "no current index";
            if (before_index) {
                auto probe = attempt<CacheEntry>("cache lookup", [&] {
                    const auto index = parse_cache_index(*before_index);
                    require(index.root == previous->root_id && index.source == request.source, "Cache source/identity changed", ErrorCode::conflict);
                    auto entry = take(read_cache_entry(paths, entry_directory(index.key, index.build), index.key, index.build));
                    auto expected = *previous; expected.unit_scale = scale;
                    require(take(serialize_asset_meta(entry.descriptor.metadata)) == take(serialize_asset_meta(expected)), "Cache settings/output mapping changed", ErrorCode::conflict);
                    verify_cache_inputs(paths, entry.artifact.inputs);
                    return entry;
                });
                if (probe) {
                    auto index = parse_cache_index(*before_index);
                    CachedAsset result{owned(index.key), owned(entry_directory(index.key, index.build)), true, {}, {}, std::move(probe->artifact)};
                    summarize(result); cache_step(CacheStep::hit);
                    verify_cache_inputs(paths, result.artifact.inputs);
                    require(optional_cache_text(persistent_path(paths, meta_name), asset_meta_byte_limit) == before_meta
                        && optional_cache_text(persistent_path(paths, current_name(index.root)), cache_index_limit) == before_index,
                        "Metadata/current changed during cache hit", ErrorCode::conflict);
                    return result;
                }
                miss = probe.error().message;
            }
        }
        cache_step(CacheStep::import);
        Vector<OutputIdentity> identities;
        if (previous) { for (const auto& o : previous->outputs) { identities.push_back({o.key, o.id, o.kind}); } }
        auto imported = take(import_gltf(paths, {request.source, identities, scale}));
        CacheDescriptor d; d.source = std::string{request.source}; d.metadata.root_id = imported.mesh.id; d.metadata.unit_scale = scale;
        for (const auto& o : imported.outputs) { d.metadata.outputs.push_back({o.key, o.id, o.kind}); }
        for (const auto& input : imported.inputs) { d.inputs.push_back({input.path, owned(content_digest(input.bytes).hex()), input.bytes.size()}); }
        for (const auto& input : d.inputs) { ordinary_cache_input(paths, input.path); }
        const auto key = take(cache_key(d)).hex(); const auto build = take(AssetId::generate()).to_string();
        const auto destination = entry_directory(key, build); const auto current = current_name(imported.mesh.id);
        if (!previous) { before_index = optional_cache_text(persistent_path(paths, current), cache_index_limit); }
        const auto encoded = take(encode_cpu_artifact(imported, d.metadata));
        const auto entry_text = Json{{"format", "DeckerAssetCacheEntry"}, {"version", 1}, {"algorithm", "xxh3-128-v1"},
            {"key", key}, {"build", build}, {"descriptor", descriptor_json(d)}, {"manifest_digest", hash_text(encoded.manifest).hex()}}.dump(2) + '\n';
        require(entry_text.size() <= cache_entry_limit, "Cache entry byte limit");
        const auto after_meta = take(serialize_asset_meta(d.metadata));
        const bool meta_changed = !previous || take(serialize_asset_meta(*previous)) != after_meta;
        const auto after_index = index_json({imported.mesh.id, std::string{request.source}, key, build});
        require(after_index.size() <= cache_index_limit, "Cache index byte limit");
        StagedEntry stage{paths, std::string{cache_root} + "/tmp/" + build, destination,
            {{{"data.bin", content_digest(encoded.bytes)}, {"manifest.json", hash_text(encoded.manifest)}, {"entry.json", hash_text(entry_text)}}}};
        directory(paths, std::string{cache_root} + "/tmp");
        directory(paths, std::string{cache_root} + "/entries/" + key);
        directory(paths, std::string{cache_root} + "/current");
        std::error_code error;
        require(fs::create_directory(persistent_path(paths, stage.temporary), error) && !error, "Cannot create cache staging", ErrorCode::io_error);
        stage.active = true; bool identity_committed = false;
        try {
            take(write_file_bytes_atomic(persistent_path(paths, stage.temporary + "/data.bin"), encoded.bytes));
            write_cache_text(persistent_path(paths, stage.temporary + "/manifest.json"), encoded.manifest);
            write_cache_text(persistent_path(paths, stage.temporary + "/entry.json"), entry_text);
            auto verified = take(read_cache_entry(paths, stage.temporary, key, build));
            CachedAsset result{owned(key), owned(destination), false, owned(miss), {}, std::move(verified.artifact)};
            summarize(result);
            const auto unchanged = [&] {
                require(optional_cache_text(persistent_path(paths, meta_name), asset_meta_byte_limit) == before_meta
                    && optional_cache_text(persistent_path(paths, current), cache_index_limit) == before_index,
                    "Metadata/current changed during cache build", ErrorCode::conflict);
            };
            cache_step(CacheStep::validate_inputs); verify_cache_inputs(paths, d.inputs); unchanged();
            take(check_asset_operations(paths.root())); cache_step(CacheStep::publish);
            move(persistent_path(paths, stage.temporary), persistent_path(paths, destination)); stage.published = true;
            cache_step(CacheStep::metadata); unchanged();
            if (meta_changed) { write_cache_text(persistent_path(paths, meta_name), after_meta); }
            identity_committed = true; stage.active = false; // Complete artifact remains if current fails.
            cache_step(CacheStep::current);
            require(optional_cache_text(persistent_path(paths, current), cache_index_limit) == before_index, "Current changed before commit", ErrorCode::conflict);
            require(optional_cache_text(persistent_path(paths, meta_name), asset_meta_byte_limit)
                == (meta_changed ? std::optional{std::string{after_meta}} : before_meta), "Metadata changed before current commit", ErrorCode::conflict);
            write_cache_text(persistent_path(paths, current), after_index);
            return result;
        } catch (const Failure& failure) {
            if (identity_committed) { throw Failure{failure.error.with_context("identity committed; current unchanged; complete artifact: " + destination)}; }
            if (!stage.cleanup()) { throw Failure{failure.error.with_context("Cache cleanup incomplete: " + std::string{stage.name()})}; }
            throw;
        }
    });
}
} // namespace dk

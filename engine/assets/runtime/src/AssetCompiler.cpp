#include <dk/assets/AssetCompiler.hpp>
#include "AssetInternal.hpp"
#include "AssetPersistenceInternal.hpp"
#include "CpuArtifactInternal.hpp"
#include "ContentDigest.hpp"
#include <dk/profiling/Profiler.hpp>
#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#endif

namespace dk {
namespace {
namespace fs = std::filesystem;
using namespace asset_detail;
thread_local const CompileHook* current_hook = nullptr;
void step(CompileStep value) { if (current_hook) { take((*current_hook)(value)); } }
bool path_exists(const fs::path& path)
{
    std::error_code error;
    const auto status = fs::symlink_status(path, error);
    if (error == std::errc::no_such_file_or_directory) { return false; }
    require(!error, "Cannot inspect path: " + error.message(), ErrorCode::io_error);
    return fs::exists(status);
}
std::optional<std::string> read_meta(const fs::path& path)
{
    if (!path_exists(path)) { return {}; }
    const auto bytes = take(read_file_bytes(path, asset_meta_byte_limit));
    return std::string{reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}
bool ancestor(const fs::path& parent, fs::path child)
{
    while (!child.empty()) {
        if (same_asset_path(parent, child)) { return true; }
        const auto next = child.parent_path();
        if (next == child) { break; }
        child = next;
    }
    return false;
}
void ordinary(const ProjectPaths& paths, std::string_view name)
{
    const auto relative = relative_path(name);
    require(!same_asset_path(*relative.begin(), fs::path{".decker"}), "Reserved .decker path");
    (void)persistent_path(paths, name);
}
// This scope owns only two known files, never arbitrary descendants of the directory.
// On an uncatchable allocation failure cleanup is best effort and does not throw.
struct Publication {
    fs::path staging, destination;
    ContentDigest data_hash, manifest_hash;
    bool active = false, published = false;
    const fs::path& path() const noexcept { return published ? destination : staging; }
    bool cleanup() noexcept
    {
        if (!active) { return true; }
        active = false;
        try {
            for (const auto& [name, expected] : {std::pair{"data.bin", data_hash}, std::pair{"manifest.json", manifest_hash}}) {
                const auto file = path() / name;
                std::error_code error;
                const auto status = fs::symlink_status(file, error);
                if (error == std::errc::no_such_file_or_directory || (!error && !fs::exists(status))) { continue; }
                if (error || !fs::is_regular_file(status) || fs::is_symlink(status)) { return false; }
                const auto actual = file_digest(file);
                if (!actual || *actual != expected) { return false; }
            }
            for (const auto* name : {"data.bin", "manifest.json"}) {
                std::error_code error; fs::remove(path() / name, error);
                if (error) { return false; }
            }
            std::error_code error;
            return fs::remove(path(), error) && !error; // An unknown file keeps the directory intact.
        } catch (...) { return false; }
    }
    ~Publication() { (void)cleanup(); }
};
void publish(const fs::path& from, const fs::path& to)
{
#ifdef _WIN32
    if (!MoveFileExW(from.c_str(), to.c_str(), 0)) {
        const std::error_code error{static_cast<int>(GetLastError()), std::system_category()};
        require(false, "Cannot publish output: " + error.message(), ErrorCode::io_error);
    }
#else
    (void)from; (void)to;
    require(false, "Asset publication requires Windows", ErrorCode::not_supported);
#endif
}
}
namespace asset_detail {
ScopedCompileHook::ScopedCompileHook(const CompileHook& hook) noexcept : previous_{current_hook} { current_hook = &hook; }
ScopedCompileHook::~ScopedCompileHook() { current_hook = previous_; }
}
Result<CompiledAsset> compile_asset(const ProjectPaths& paths, const AssetCompileRequest& request)
{
    DK_PROFILE_ZONE("Assets.Compile");
    return attempt<CompiledAsset>("compile_asset", [&] {
        take(check_asset_operations(paths.root()));
        ordinary(paths, request.source); ordinary(paths, request.output_directory);
        const auto source = persistent_path(paths, request.source);
        const auto meta_name = std::string{request.source} + ".meta";
        const auto meta_path = persistent_path(paths, meta_name);
        const auto output = persistent_path(paths, request.output_directory);
        require(!path_exists(output), "Output directory already exists", ErrorCode::conflict);
        require(!ancestor(output, source) && !ancestor(output, meta_path), "Output contains source or metadata");
        std::error_code error;
        require(fs::is_directory(output.parent_path(), error) && !error, "Output parent must exist", ErrorCode::io_error);
        const auto before = read_meta(meta_path);
        std::optional<AssetMetadata> previous;
        Vector<OutputIdentity> identities;
        if (before) {
            previous = take(parse_asset_meta(*before));
            for (const auto& item : previous->outputs) { identities.push_back({item.key, item.id, item.kind}); }
        }
        const auto scale = request.unit_scale.value_or(previous ? previous->unit_scale : 1);
        const auto imported = take(import_gltf(paths, {request.source, identities, scale, request.limits}));
        AssetMetadata metadata{imported.mesh.id, scale, {}};
        for (const auto& item : imported.outputs) { metadata.outputs.push_back({item.key, item.id, item.kind}); }
        const auto after = take(serialize_asset_meta(metadata));
        const bool meta_changed = !previous || take(serialize_asset_meta(*previous)) != after;
        auto encoded = take(encode_cpu_artifact(imported, metadata));
        const auto manifest_bytes = std::as_bytes(std::span{encoded.manifest.data(), encoded.manifest.size()});
        Publication publication{output.parent_path() / (".dk-asset-" + take(AssetId::generate()).to_string()), output,
            content_digest(encoded.bytes), content_digest(manifest_bytes)};
        CompiledAsset result{metadata.root_id, owned(request.output_directory), imported.outputs, {}};
        nlohmann::json outputs = nlohmann::json::array();
        for (const auto& item : imported.outputs) {
            outputs.push_back({{"key", std::string{item.key}}, {"id", item.id.to_string()}, {"kind", asset_kind_name(item.kind)}});
        }
        nlohmann::json inputs = nlohmann::json::array();
        for (const auto& input : imported.inputs) {
            inputs.push_back({{"path", std::string{input.path}}, {"bytes", input.bytes.size()}, {"digest", content_digest(input.bytes).hex()}});
        }
        nlohmann::json diagnostics = nlohmann::json::array();
        for (const auto& item : imported.diagnostics) { diagnostics.push_back(std::string{item}); }
        result.summary_json = owned(nlohmann::json{{"format", "DeckerAssetCompileResult"}, {"version", 1},
            {"source", request.source}, {"output", request.output_directory}, {"root_id", metadata.root_id.to_string()},
            {"unit_scale", scale}, {"inputs", std::move(inputs)}, {"outputs", std::move(outputs)},
            {"diagnostics", std::move(diagnostics)}}.dump() + '\n');
        // All owned return values and encoded bytes are allocated before filesystem publication.
        require(fs::create_directory(publication.staging, error) && !error, "Cannot create staging directory: " + error.message(), ErrorCode::io_error);
        publication.active = true;
        try {
            take(write_file_bytes_atomic(publication.staging / "data.bin", encoded.bytes));
            take(write_file_bytes_atomic(publication.staging / "manifest.json", manifest_bytes));
            (void)take(load_cpu_artifact(publication.staging));
            step(CompileStep::validate_inputs);
            for (const auto& input : imported.inputs) {
                ordinary(paths, input.path);
                const auto path = persistent_path(paths, input.path);
                require(!ancestor(output, path), "Output contains an input");
                require(take(file_digest(path)) == content_digest(input.bytes), "Input changed during import: " + std::string{input.path}, ErrorCode::conflict);
            }
            take(check_asset_operations(paths.root()));
            require(read_meta(persistent_path(paths, meta_name)) == before, "Metadata changed during import", ErrorCode::conflict);
            step(CompileStep::publish);
            (void)persistent_path(paths, request.output_directory);
            publish(publication.staging, output); publication.published = true;
            step(CompileStep::metadata);
            require(read_meta(persistent_path(paths, meta_name)) == before, "Metadata changed before commit", ErrorCode::conflict);
            if (meta_changed) {
                take(write_file_bytes_atomic(meta_path, std::as_bytes(std::span{after.data(), after.size()}),
                    [&](const fs::path& temporary) -> Result<void> {
                        return attempt<void>("verify compiled metadata", [&] {
                            require(read_meta(temporary) == std::optional<std::string>{std::string{after}}, "Metadata write differs", ErrorCode::io_error);
                        });
                    }));
            }
            publication.active = false; // Final commit: nothing fallible follows.
            return result;
        } catch (const Failure& failure) {
            if (!publication.cleanup()) {
                throw Failure{failure.error.with_context("Cleanup incomplete; preserved unowned/changed directory: "
                    + take(path_to_utf8(publication.path())))};
            }
            throw;
        }
    });
}
} // namespace dk

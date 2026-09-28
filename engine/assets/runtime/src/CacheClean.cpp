#include "AssetCacheInternal.hpp"
#include <dk/profiling/Profiler.hpp>
#include <algorithm>
#include <map>
#include <set>

namespace dk {
namespace {
using namespace asset_detail;
namespace fs = std::filesystem;
std::vector<std::string> children(const ProjectPaths& paths, const std::string& directory, std::size_t& budget)
{
    const auto path = persistent_path(paths, directory);
    if (!cache_path_exists(path)) { return {}; }
    std::error_code error;
    fs::directory_iterator iterator{path, error};
    require(!error, "Cannot enumerate cache: " + error.message(), ErrorCode::io_error);
    std::vector<std::string> result;
    for (const auto end = fs::directory_iterator{}; iterator != end; iterator.increment(error)) {
        require(!error, "Cannot enumerate cache: " + error.message(), ErrorCode::io_error);
        require(budget > 0, "Cache cleanup scan budget exceeded"); --budget;
        result.push_back(take(path_to_utf8(iterator->path().filename())));
    }
    require(!error, "Cannot enumerate cache: " + error.message(), ErrorCode::io_error);
    std::sort(result.begin(), result.end()); return result;
}
using IndexSnapshot = std::map<std::string, std::string>;
IndexSnapshot indexes(const ProjectPaths& paths, std::size_t limit, std::set<std::string>* live = nullptr)
{
    const auto directory = std::string{cache_root} + "/current";
    IndexSnapshot result;
    for (const auto& name : children(paths, directory, limit)) {
        const auto text = read_cache_text(persistent_path(paths, directory + '/' + name), cache_index_limit);
        const auto index = parse_cache_index(text);
        require(name == index.root.to_string() + ".json", "Unrecognized current index filename; cleanup refused");
        ordinary_cache_input(paths, index.source);
        if (live) { live->insert(entry_directory(index.key, index.build)); }
        result.emplace(name, text);
    }
    return result;
}
constexpr std::array<std::string_view, 3> entry_files{"data.bin", "entry.json", "manifest.json"};
bool exact_files(const ProjectPaths& paths, const std::string& directory)
{
    std::size_t budget = 4;
    const auto names = children(paths, directory, budget);
    return names.size() == entry_files.size() && std::equal(names.begin(), names.end(), entry_files.begin());
}
bool is_key(std::string_view key)
{
    return key.size() == 32 && std::all_of(key.begin(), key.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}
struct Candidate {
    std::string directory;
    std::array<ContentDigest, 3> hashes;
};
std::array<ContentDigest, 3> fingerprints(const ProjectPaths& paths, const std::string& directory)
{
    std::array<ContentDigest, 3> result;
    for (std::size_t i = 0; i < entry_files.size(); ++i) {
        const auto file = persistent_path(paths, directory + '/' + std::string{entry_files[i]});
        const auto limit = i == 0 ? 256 * 1024 * 1024 : cache_entry_limit;
        result[i] = content_digest(take(read_file_bytes(file, limit)));
    }
    return result;
}
}
Result<CacheCleanResult> clean_asset_cache(const ProjectPaths& paths, std::size_t scan_limit)
{
    DK_PROFILE_ZONE("Assets.CacheClean");
    return attempt<CacheCleanResult>("clean_asset_cache", [&] {
        require(scan_limit > 0 && scan_limit <= 10000, "Invalid cache cleanup budget");
        take(check_asset_operations(paths.root()));
        std::set<std::string> live;
        const auto before = indexes(paths, scan_limit, &live);
        CacheCleanResult result; std::vector<Candidate> candidates;
        const auto entries = std::string{cache_root} + "/entries";
        auto budget = scan_limit;
        for (const auto& key : children(paths, entries, budget)) {
            const auto bucket = entries + '/' + key;
            if (!is_key(key)) { ++result.skipped; result.diagnostics.push_back(owned("Unknown cache path retained: " + bucket)); continue; }
            // Invalid/reparse key buckets are preserved. Enumeration budget errors propagate.
            const auto valid_bucket = attempt<bool>("cache bucket", [&] {
                std::error_code error; const auto valid = fs::is_directory(persistent_path(paths, bucket), error);
                require(!error && valid, "Not an ordinary cache directory"); return true;
            });
            if (!valid_bucket) { ++result.skipped; result.diagnostics.push_back(owned("Unsafe cache bucket retained: " + bucket)); continue; }
            for (const auto& build : children(paths, bucket, budget)) {
                const auto directory = bucket + '/' + build;
                if (live.contains(directory)) { ++result.retained; continue; }
                auto candidate = attempt<Candidate>("cache cleanup candidate", [&] {
                    require(entry_directory(key, build) == directory, "Invalid cache build name");
                    require(exact_files(paths, directory), "Unknown files in cache entry");
                    (void)take(read_cache_entry(paths, directory, key, build));
                    return Candidate{directory, fingerprints(paths, directory)};
                });
                if (candidate) { candidates.push_back(std::move(*candidate)); }
                else { ++result.skipped; result.diagnostics.push_back(owned(directory + ": " + candidate.error().message)); }
            }
        }
        cache_step(CacheStep::clean);
        take(check_asset_operations(paths.root()));
        require(indexes(paths, scan_limit) == before, "Current indexes changed; cleanup refused", ErrorCode::conflict);
        for (const auto& candidate : candidates) {
            // Recheck before touching the entry. Each remove is restricted to a verified ordinary file.
            const auto unchanged = attempt<void>("cache cleanup preflight", [&] {
                require(exact_files(paths, candidate.directory) && fingerprints(paths, candidate.directory) == candidate.hashes,
                    "Cache entry changed; retained", ErrorCode::conflict);
            });
            if (!unchanged) { ++result.skipped; result.diagnostics.push_back(owned(candidate.directory + ": " + unchanged.error().message)); continue; }
            const auto erased = attempt<void>("cache cleanup remove", [&] {
                for (const auto file : entry_files) {
                    cache_step(CacheStep::clean_file);
                    std::error_code error;
                    const auto removed = fs::remove(persistent_path(paths, candidate.directory + '/' + std::string{file}), error);
                    require(removed && !error, "Cannot remove cache file: " + error.message(), ErrorCode::io_error);
                }
                std::error_code error;
                require(fs::remove(persistent_path(paths, candidate.directory), error) && !error, "Cannot remove empty cache directory", ErrorCode::io_error);
            });
            if (erased) { ++result.removed; }
            else { ++result.failed; result.diagnostics.push_back(owned(candidate.directory + ": partial cleanup: " + erased.error().message)); }
        }
        auto diagnostics = nlohmann::json::array();
        for (const auto& value : result.diagnostics) { diagnostics.push_back(std::string{value}); }
        result.summary_json = owned(nlohmann::json{{"format", "DeckerAssetCacheCleanResult"}, {"version", 1},
            {"removed", result.removed}, {"retained", result.retained}, {"skipped", result.skipped}, {"failed", result.failed},
            {"diagnostics", std::move(diagnostics)}}.dump() + '\n');
        return result;
    });
}
} // namespace dk

#pragma once
#include <dk/assets/CpuArtifact.hpp>
#include <dk/io/Path.hpp>
#include <optional>

namespace dk {
struct AssetCacheRequest { std::string_view source; std::optional<double> unit_scale; };
struct CachedAsset {
    String key, directory;
    bool cache_hit = false;
    String miss_reason, summary_json;
    CpuArtifact artifact;
};
// Synchronous, fixed importer budgets; caller binds persistent memory and thread scratch.
// Serialize writes per project. A current-write failure can retain committed meta + complete
// unreferenced data, explicitly reported in Error.context; existing current is never truncated.
[[nodiscard]] Result<CachedAsset> compile_cached_asset(const ProjectPaths&, const AssetCacheRequest&);
struct CacheCleanResult {
    std::size_t removed = 0, retained = 0, skipped = 0, failed = 0;
    Vector<String> diagnostics;
    String summary_json;
};
// Explicitly reclaims valid unreferenced entries only. Unknown/current/modified entries stay.
// Bad indexes/budget overflow reject before deletion; per-entry IO failure is reported in failed.
[[nodiscard]] Result<CacheCleanResult> clean_asset_cache(const ProjectPaths&, std::size_t scan_limit = 10000);
} // namespace dk

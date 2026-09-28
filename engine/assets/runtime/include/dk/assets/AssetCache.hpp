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
} // namespace dk

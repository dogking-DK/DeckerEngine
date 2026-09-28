#pragma once
#include <dk/assets/AssetCache.hpp>
#include <dk/jobs/JobQueue.hpp>

namespace dk {
enum class AssetState { unloaded, loading, ready, failed };
struct AssetStatus {
    AssetId id;
    AssetState state = AssetState::unloaded;
    std::uint64_t generation = 0, ready_generation = 0;
    std::optional<JobId> job;
    std::shared_ptr<const CachedAsset> data;
    std::optional<Error> error;
};
// Single owner thread, exclusive completion consumer of the borrowed queue.
// Destruction cancels requests; owner must close/join queue before destroying memory.
class AsyncAssets final {
public:
    using Prepare = std::function<Result<PreparedCachedAsset>(const ProjectPaths&, const AssetCacheRequest&, std::stop_token)>;
    AsyncAssets(ProjectPaths paths, JobQueue& queue, Prepare prepare = prepare_cached_asset);
    ~AsyncAssets();
    AsyncAssets(const AsyncAssets&) = delete;
    AsyncAssets& operator=(const AsyncAssets&) = delete;
    [[nodiscard]] Result<JobId> import(std::string_view source, std::optional<double> scale = {});
    [[nodiscard]] Result<AssetStatus> load(AssetId id, std::string_view registered_source);
    [[nodiscard]] AssetStatus status(AssetId id, std::string_view registered_source) const;
    [[nodiscard]] Result<AssetStatus> unload(AssetId id, std::string_view registered_source);
    [[nodiscard]] Result<void> reset_session();
    void pump();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace dk

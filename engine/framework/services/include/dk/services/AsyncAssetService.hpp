#pragma once
#include <dk/assets/AsyncAssets.hpp>
#include <dk/services/AssetService.hpp>
#include <dk/services/SceneService.hpp>

namespace dk {
class AsyncAssetService final {
public:
    [[nodiscard]] static Result<std::unique_ptr<AsyncAssetService>> create(const std::filesystem::path&, std::function<void()> wake = {});
    ~AsyncAssetService();
    [[nodiscard]] Result<void> open(std::string_view manifest, std::optional<CatalogGuard> guard = {});
    [[nodiscard]] Result<const AssetService*> catalog() const;
    [[nodiscard]] Result<void> register_source(CatalogGuard, const RegistrationRequest&);
    [[nodiscard]] Result<void> rename_source(CatalogGuard, std::string_view source, std::string_view target);
    [[nodiscard]] Result<JobId> import(std::string_view source, std::optional<double> scale = {});
    [[nodiscard]] Result<AssetStatus> load(AssetId);
    [[nodiscard]] Result<AssetStatus> status(AssetId);
    [[nodiscard]] Result<AssetStatus> unload(AssetId);
    [[nodiscard]] Result<JobSnapshot> job(JobId) const;
    [[nodiscard]] Result<JobWait> wait(JobId, std::chrono::milliseconds timeout);
    [[nodiscard]] Result<JobCancel> cancel(JobId);
    [[nodiscard]] JobLimits limits() const noexcept;
    [[nodiscard]] Result<void> synchronize_scene(SceneService&, std::optional<std::string_view> saving_to = {}) const;
    [[nodiscard]] Result<void> refresh_manifest(std::string_view manifest);
    void pump();
    void rethrow_failure() const;
    void close() noexcept;
private:
    struct Impl;
    explicit AsyncAssetService(std::unique_ptr<Impl>);
    std::unique_ptr<Impl> impl_;
};
}

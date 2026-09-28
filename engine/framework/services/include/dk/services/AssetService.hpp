#pragma once
#include <dk/services/AssetRegistration.hpp>
#include <memory>

namespace dk {
// Synchronous single-writer owner. Open only after explicit recovery succeeds.
// Callers bind a persistent Assets memory context. Borrowed views expire on mutation.
class AssetService {
public:
    [[nodiscard]] static Result<AssetService> open(const std::filesystem::path& root, std::string_view manifest);
    AssetService(AssetService&&) noexcept = default;
    AssetService& operator=(AssetService&&) noexcept = default;
    [[nodiscard]] const Project& project() const noexcept { return *project_; }
    [[nodiscard]] const AssetCatalog& catalog() const noexcept { return catalog_; }
    [[nodiscard]] Result<void> register_source(CatalogGuard guard, const RegistrationRequest& request);
    [[nodiscard]] Result<void> rename_source(CatalogGuard guard, std::string_view source, std::string_view target);
private:
    [[nodiscard]] Result<void> check_scene_paths(std::string_view source, std::string_view target) const;
    AssetService(std::unique_ptr<Project> project, AssetCatalog catalog, String manifest, String bytes)
        : project_(std::move(project)), catalog_(std::move(catalog)), manifest_(std::move(manifest)), bytes_(std::move(bytes)) {}
    std::unique_ptr<Project> project_;
    AssetCatalog catalog_;
    String manifest_, bytes_;
};
} // namespace dk

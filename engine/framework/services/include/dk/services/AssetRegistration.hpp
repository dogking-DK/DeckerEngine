#pragma once
#include <dk/assets/Catalog.hpp>
#include <dk/scene/Project.hpp>

namespace dk {
struct ProjectRegistrationCandidate {
    RegistrationCandidate registration;
    Project project;
};
// Pure adaptation/preparation. Does not mutate Project, Scene, catalog or disk.
[[nodiscard]] Result<AssetCatalog> make_asset_catalog(const Project& project);
[[nodiscard]] Result<ProjectRegistrationCandidate> prepare_asset_registration(const Project& project,
    const AssetCatalog& catalog, CatalogGuard guard, const RegistrationRequest& request);
} // namespace dk

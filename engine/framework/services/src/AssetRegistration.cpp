#include <dk/services/AssetRegistration.hpp>
#include <algorithm>

namespace dk {
Result<AssetCatalog> make_asset_catalog(const Project& project)
{
    Vector<AssetCatalogRecord> records;
    records.reserve(project.description().assets.size());
    for (const auto& record : project.description().assets) {
        records.push_back({record.id, record.kind, String{record.path.begin(), record.path.end()}});
    }
    return AssetCatalog::create(project.paths().root(), records);
}
Result<ProjectRegistrationCandidate> prepare_asset_registration(const Project& project, const AssetCatalog& catalog,
    CatalogGuard guard, const RegistrationRequest& request)
{
    const auto& current = project.description().assets;
    if (project.paths().root() != catalog.paths().root() || current.size() != catalog.records().size()) {
        return std::unexpected(Error{ErrorCode::conflict, "Project and catalog snapshots differ"});
    }
    for (const auto& record : current) {
        const auto records = catalog.records();
        const auto found = std::lower_bound(records.begin(), records.end(), record.id,
            [](const auto& item, const auto& id) { return item.id < id; });
        if (found == records.end() || found->id != record.id || found->kind != record.kind || std::string_view{found->path} != record.path) {
            return std::unexpected(Error{ErrorCode::conflict, "Project record differs from catalog", {record.id.to_string()}});
        }
    }
    auto prepared = catalog.prepare_registration(guard, request);
    if (!prepared) { return std::unexpected(prepared.error()); }
    auto description = project.description();
    description.assets.clear(); description.assets.reserve(prepared->records().size());
    for (const auto& record : prepared->records()) { description.assets.push_back({record.id, record.kind, std::string{record.path}}); }
    auto candidate = Project::create(project.paths().root(), std::move(description));
    if (!candidate) { return std::unexpected(candidate.error().with_context("prepare_asset_registration")); }
    return ProjectRegistrationCandidate{std::move(*prepared), std::move(*candidate)};
}
} // namespace dk

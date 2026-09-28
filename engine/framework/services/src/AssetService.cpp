#include <dk/services/AssetService.hpp>
#include <dk/io/File.hpp>
#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#endif

namespace dk {
Result<void> AssetService::check_scene_paths(std::string_view source, std::string_view target) const
{
    const auto scene = project_->scene_path();
    if (!scene) { return std::unexpected(scene.error()); }
    for (const auto& name : {std::string{source}, std::string{source} + ".meta", std::string{target},
            std::string{target} + ".meta", std::string{manifest_}}) {
        const auto relative = path_from_utf8(name);
        if (!relative) { return std::unexpected(relative.error()); }
        const auto path = project_->paths().resolve(*relative);
        if (!path) { return std::unexpected(path.error()); }
#ifdef _WIN32
        const bool same = CompareStringOrdinal(scene->c_str(), static_cast<int>(scene->native().size()),
            path->c_str(), static_cast<int>(path->native().size()), TRUE) == CSTR_EQUAL;
#else
        const bool same = *scene == *path;
#endif
        if (same) { return std::unexpected(Error{ErrorCode::conflict, "Asset operation overlaps the Scene path", {name}}); }
    }
    return {};
}
Result<AssetService> AssetService::open(const std::filesystem::path& root, std::string_view manifest)
{
    const auto checked = check_asset_operations(root);
    if (!checked) { return std::unexpected(checked.error()); }
    const auto relative = path_from_utf8(manifest);
    if (!relative) { return std::unexpected(relative.error()); }
    const auto paths = ProjectPaths::create(root);
    if (!paths) { return std::unexpected(paths.error()); }
    const auto path = paths->resolve(*relative);
    if (!path) { return std::unexpected(path.error()); }
    const auto bytes = read_file_bytes(*path, 16 * 1024 * 1024);
    if (!bytes) { return std::unexpected(bytes.error()); }
    const std::string_view text{reinterpret_cast<const char*>(bytes->data()), bytes->size()};
    auto description = parse_project(text);
    if (!description) { return std::unexpected(description.error()); }
    auto project = Project::create(root, std::move(*description));
    if (!project) { return std::unexpected(project.error()); }
    auto catalog = make_asset_catalog(*project);
    if (!catalog) { return std::unexpected(catalog.error()); }
    return AssetService{std::make_unique<Project>(std::move(*project)), std::move(*catalog),
        String{manifest.begin(), manifest.end()}, String{text.begin(), text.end()}};
}
Result<void> AssetService::register_source(CatalogGuard guard, const RegistrationRequest& request)
{
    const auto checked = check_scene_paths(request.source, request.source);
    if (!checked) { return checked; }
    auto candidate = prepare_asset_registration(*project_, catalog_, guard, request);
    if (!candidate) { return std::unexpected(candidate.error()); }
    const bool changed = candidate->registration.changed();
    auto serialized = serialize_project(candidate->project.description());
    if (!serialized) { return std::unexpected(serialized.error()); }
    String next_bytes{serialized->begin(), serialized->end()};
    auto project = std::make_unique<Project>(std::move(candidate->project));
    const auto committed = catalog_.commit_registration(std::move(candidate->registration), {manifest_, bytes_, next_bytes});
    if (!committed) { return committed; }
    if (changed) { project_.swap(project); bytes_.swap(next_bytes); }
    return {};
}
Result<void> AssetService::refresh_manifest()
{
    auto relative = path_from_utf8(manifest_); if (!relative) return std::unexpected(relative.error());
    auto path = project_->paths().resolve(*relative); if (!path) return std::unexpected(path.error());
    auto bytes = read_file_bytes(*path, 16 * 1024 * 1024); if (!bytes) return std::unexpected(bytes.error());
    const std::string_view text{reinterpret_cast<const char*>(bytes->data()),bytes->size()};
    auto parsed = parse_project(text); if (!parsed) return std::unexpected(parsed.error());
    auto actual = serialize_project(*parsed), expected = serialize_project(project_->description());
    if (!actual) return std::unexpected(actual.error()); if (!expected) return std::unexpected(expected.error());
    if (*actual != *expected) return std::unexpected(Error{ErrorCode::conflict,"Project changed; reopen the asset catalog"});
    String next{text.begin(),text.end()}; bytes_.swap(next); return {};
}
Result<void> AssetService::rename_source(CatalogGuard guard, std::string_view source, std::string_view target)
{
    const auto checked = check_scene_paths(source, target);
    if (!checked) { return checked; }
    auto candidate = catalog_.prepare_rename(guard, source, target);
    if (!candidate) { return std::unexpected(candidate.error()); }
    auto description = project_->description();
    description.assets.clear(); description.assets.reserve(candidate->records().size());
    for (const auto& record : candidate->records()) { description.assets.push_back({record.id, record.kind, std::string{record.path}}); }
    auto project = Project::create(project_->paths().root(), std::move(description));
    if (!project) { return std::unexpected(project.error()); }
    auto serialized = serialize_project(project->description());
    if (!serialized) { return std::unexpected(serialized.error()); }
    String next_bytes{serialized->begin(), serialized->end()};
    auto next_project = std::make_unique<Project>(std::move(*project));
    const auto committed = catalog_.commit_rename(std::move(*candidate), {manifest_, bytes_, next_bytes});
    if (!committed) { return committed; }
    project_.swap(next_project); bytes_.swap(next_bytes);
    return {};
}
} // namespace dk

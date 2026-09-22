#include <dk/scene/Project.hpp>
#include <dk/scene/SceneDocument.hpp>
#include <dk/io/File.hpp>
#include "JsonSupport.hpp"
#include <algorithm>

namespace dk {
namespace {
using namespace detail;
void relative_path(std::string_view text)
{
    require(!text.empty() && text.size() <= 4096 && text.find_first_of("\\:") == std::string_view::npos,
        "Expected a portable relative path");
    const auto path = path_from_utf8(text);
    require(text.find("//") == std::string_view::npos, "Empty path segment");
    require(path.has_value() && !path->has_root_path(), "Invalid relative UTF-8 path");
    for (const auto& part : *path) {
        require(!part.empty() && part != "." && part != "..", "Path must be normalized and stay inside project");
    }
}
void validate_description(const ProjectDescription& description)
{
    require(!description.name.empty() && description.name.size() <= 1024 && description.name.find('\0') == std::string::npos,
        "Project name must contain 1-1024 UTF-8 bytes without NUL");
    (void)Json(description.name).dump(); // Strict UTF-8 check on caller-owned strings.
    relative_path(description.scene);
    require(description.assets.size() <= 10000, "Too many asset records");
    std::unordered_set<AssetId> ids;
    for (const auto& asset : description.assets) {
        require(!asset.id.is_nil() && ids.insert(asset.id).second, "Invalid or duplicate AssetId");
        require(!asset_kind_name(asset.kind).empty(), "Unknown asset kind");
        relative_path(asset.path);
    }
}
Result<void> regular_file(const std::filesystem::path& path)
{
    std::error_code error;
    const auto status = std::filesystem::status(path, error);
    const auto text = path_to_utf8(path);
    const auto context = text ? *text : "<path>";
    if (error == std::errc::no_such_file_or_directory || (!error && !std::filesystem::exists(status))) {
        return std::unexpected(Error{ErrorCode::not_found, "Project file not found", {context}});
    }
    if (error) { return std::unexpected(Error{ErrorCode::io_error, error.message(), {context}}); }
    if (!std::filesystem::is_regular_file(status)) {
        return std::unexpected(Error{ErrorCode::invalid_argument, "Project resource is not a regular file", {context}});
    }
    return {};
}
} // namespace

Result<ProjectDescription> parse_project(std::string_view text)
{
    try {
        const auto json = detail::parse_json(text);
        detail::fields(json, {"format", "version", "name", "scene", "assets"});
        detail::require(json.at("format") == "DeckerProject", "Unknown project format");
        detail::version_one(json.at("version"));
        ProjectDescription description{detail::string_value(json.at("name")), detail::string_value(json.at("scene")), {}};
        const auto& assets = json.at("assets");
        detail::require(assets.is_array() && assets.size() <= 10000, "Invalid asset array");
        for (const auto& item : assets) {
            detail::fields(item, {"id", "kind", "path"});
            const auto kind = parse_asset_kind(detail::string_value(item.at("kind")));
            detail::require(kind.has_value(), "Unknown asset kind");
            description.assets.push_back({detail::read_id<AssetId>(item.at("id")), *kind, detail::string_value(item.at("path"))});
        }
        validate_description(description);
        return description;
    } catch (const detail::FormatError& error) { return std::unexpected(detail::format_error(error, "parse_project")); }
      catch (const detail::Json::exception& error) { return std::unexpected(detail::json_error(error, "parse_project")); }
}

Result<std::string> serialize_project(const ProjectDescription& description)
{
    try {
        validate_description(description);
        auto assets = description.assets;
        std::sort(assets.begin(), assets.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
        auto records = detail::Json::array();
        for (const auto& asset : assets) {
            records.push_back({{"id", asset.id.to_string()}, {"kind", asset_kind_name(asset.kind)}, {"path", asset.path}});
        }
        detail::Json json{{"format", "DeckerProject"}, {"version", 1}, {"name", description.name},
            {"scene", description.scene}, {"assets", std::move(records)}};
        auto text = json.dump(2) + "\n";
        detail::require(text.size() <= detail::json_limit, "Project exceeds JSON size limit");
        return text;
    } catch (const detail::FormatError& error) { return std::unexpected(detail::format_error(error, "serialize_project")); }
      catch (const detail::Json::exception& error) { return std::unexpected(detail::json_error(error, "serialize_project")); }
}

Project::Project(ProjectPaths paths, ProjectDescription description)
    : paths_{std::move(paths)}, description_{std::move(description)}
{
    for (std::size_t i = 0; i < description_.assets.size(); ++i) { index_.emplace(description_.assets[i].id, i); }
}
Result<Project> Project::create(const std::filesystem::path& root, ProjectDescription description)
{
    const auto validated = serialize_project(description);
    if (!validated) { return std::unexpected(validated.error()); }
    auto paths = ProjectPaths::create(root);
    if (!paths) { return std::unexpected(paths.error()); }
    return Project{std::move(*paths), std::move(description)};
}
Result<Project> Project::open(const std::filesystem::path& root, const std::filesystem::path& manifest)
{
    const auto paths = ProjectPaths::create(root);
    if (!paths) { return std::unexpected(paths.error()); }
    const auto path = paths->resolve(manifest);
    if (!path) { return std::unexpected(path.error()); }
    const auto bytes = read_file_bytes(*path, detail::json_limit);
    if (!bytes) { return std::unexpected(bytes.error()); }
    std::string text;
    if (!bytes->empty()) { text.assign(reinterpret_cast<const char*>(bytes->data()), bytes->size()); }
    auto description = parse_project(text);
    if (!description) { return std::unexpected(description.error().with_context("Project.open")); }
    return create(root, std::move(*description));
}
Result<std::filesystem::path> Project::scene_path() const
{
    const auto relative = path_from_utf8(description_.scene);
    if (!relative) { return std::unexpected(relative.error()); }
    return paths_.resolve(*relative);
}
Result<std::filesystem::path> Project::resolve_asset(AssetReference reference) const
{
    const auto valid = validate_asset_references(std::span{&reference, 1});
    if (!valid) { return std::unexpected(valid.error()); }
    const auto found = index_.find(reference.id);
    if (found == index_.end()) {
        return std::unexpected(Error{ErrorCode::not_found, "AssetId is not registered", {reference.id.to_string()}});
    }
    const auto& asset = description_.assets[found->second];
    if (asset.kind != reference.kind) {
        return std::unexpected(Error{ErrorCode::invalid_argument, "Asset kind mismatch", {asset.id.to_string(), asset.path}});
    }
    const auto relative = path_from_utf8(asset.path);
    if (!relative) { return std::unexpected(relative.error()); }
    const auto path = paths_.resolve(*relative);
    if (!path) { return std::unexpected(path.error()); }
    const auto checked = regular_file(*path);
    if (!checked) { return std::unexpected(checked.error().with_context(asset.id.to_string())); }
    return *path;
}
Result<void> Project::validate_files() const
{
    const auto scene = scene_path();
    if (!scene) { return std::unexpected(scene.error()); }
    const auto checked = regular_file(*scene);
    if (!checked) { return checked; }
    for (const auto& asset : description_.assets) {
        const auto path = resolve_asset({asset.id, asset.kind});
        if (!path) { return std::unexpected(path.error()); }
    }
    return {};
}
Result<void> check_asset_references(const SceneDocument& scene, const Project& project)
{
    for (const auto id : scene.entity_ids()) {
        const auto entity = scene.entity(id);
        if (!entity) { return std::unexpected(entity.error()); }
        for (const auto& reference : entity->assets) {
            const auto path = project.resolve_asset(reference);
            if (!path) { return std::unexpected(path.error().with_context(id.to_string())); }
        }
    }
    return {};
}
} // namespace dk

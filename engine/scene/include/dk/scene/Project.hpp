#pragma once
#include <dk/assets/AssetReference.hpp>
#include <dk/io/Path.hpp>
#include <string>
#include <unordered_map>
#include <vector>

namespace dk {
struct AssetRecord {
    AssetId id;
    AssetKind kind = AssetKind::mesh;
    std::string path;
    auto operator<=>(const AssetRecord&) const = default;
};
struct ProjectDescription {
    std::string name;
    std::string scene;
    std::vector<AssetRecord> assets;
};
[[nodiscard]] Result<ProjectDescription> parse_project(std::string_view text);
[[nodiscard]] Result<std::string> serialize_project(const ProjectDescription& description);

class SceneDocument;
class Project {
public:
    [[nodiscard]] static Result<Project> create(const std::filesystem::path& root, ProjectDescription description);
    [[nodiscard]] static Result<Project> open(const std::filesystem::path& root, const std::filesystem::path& manifest);
    [[nodiscard]] const ProjectDescription& description() const noexcept { return description_; }
    [[nodiscard]] const ProjectPaths& paths() const noexcept { return paths_; }
    [[nodiscard]] Result<std::filesystem::path> scene_path() const;
    [[nodiscard]] Result<std::filesystem::path> resolve_asset(AssetReference reference) const;
    [[nodiscard]] Result<void> validate_files() const;
private:
    Project(ProjectPaths paths, ProjectDescription description);
    ProjectPaths paths_;
    ProjectDescription description_;
    std::unordered_map<AssetId, std::size_t> index_;
};
[[nodiscard]] Result<void> check_asset_references(const SceneDocument& scene, const Project& project);
} // namespace dk

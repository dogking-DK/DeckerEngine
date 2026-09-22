#pragma once
#include <dk/scene/SceneDocument.hpp>
#include <dk/scene/Project.hpp>

namespace dk {
// Memory imports are dirty. File loads establish a clean baseline.
[[nodiscard]] Result<std::string> serialize_scene(const SceneSnapshot& snapshot, const Project& project);
[[nodiscard]] Result<std::unique_ptr<SceneDocument>> parse_scene(std::string_view text, const Project& project);
[[nodiscard]] Result<std::unique_ptr<SceneDocument>> load_scene(const Project& project);
// Replaces the owner only on success; all references to the old document then expire.
[[nodiscard]] Result<void> reload_scene(std::unique_ptr<SceneDocument>& document, const Project& project);
[[nodiscard]] Result<void> save_scene(SceneDocument& document, const SceneSnapshot& snapshot, const Project& project);
[[nodiscard]] Result<void> save_scene(SceneDocument& document, const Project& project);
[[nodiscard]] Result<void> save_project(const Project& project, const std::filesystem::path& relative_manifest);
} // namespace dk

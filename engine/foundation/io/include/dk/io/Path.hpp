#pragma once

#include <dk/core/Result.hpp>

#include <filesystem>
#include <string>
#include <string_view>
#include <utility>

namespace dk {

[[nodiscard]] Result<std::filesystem::path> path_from_utf8(std::string_view text);
[[nodiscard]] Result<std::string> path_to_utf8(const std::filesystem::path& path);

class ProjectPaths {
public:
    // Captures an existing directory as a canonical absolute root.
    [[nodiscard]] static Result<ProjectPaths> create(const std::filesystem::path& root);
    [[nodiscard]] const std::filesystem::path& root() const noexcept { return root_; }
    // Lexical resolution only; child symlinks may refer outside the root.
    [[nodiscard]] Result<std::filesystem::path> resolve(const std::filesystem::path& relative) const;

private:
    explicit ProjectPaths(std::filesystem::path root) : root_{std::move(root)} {}
    std::filesystem::path root_;
};

} // namespace dk

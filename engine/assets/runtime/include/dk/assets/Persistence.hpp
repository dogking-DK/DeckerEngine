#pragma once
#include <dk/core/Result.hpp>
#include <filesystem>
#include <string_view>

namespace dk {
// Opaque to assets/runtime; the services adapter validates Project semantics.
// The manifest must already exist and match before exactly.
struct AssetManifestUpdate { std::string_view path, before, after; };
[[nodiscard]] Result<void> check_asset_operations(const std::filesystem::path& root);
// Rolls back an unfinished operation only if every affected file is recognized.
// A service that required recovery must be reopened afterwards.
[[nodiscard]] Result<void> recover_asset_operations(const std::filesystem::path& root);
} // namespace dk

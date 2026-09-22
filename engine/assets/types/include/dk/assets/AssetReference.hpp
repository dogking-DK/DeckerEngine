#pragma once
#include <dk/core/StableId.hpp>
#include <span>
#include <string_view>

namespace dk {
enum class AssetKind { mesh, material, texture };
struct AssetReference {
    AssetId id;
    AssetKind kind = AssetKind::mesh;
    auto operator<=>(const AssetReference&) const = default;
};
[[nodiscard]] std::string_view asset_kind_name(AssetKind kind) noexcept;
[[nodiscard]] Result<AssetKind> parse_asset_kind(std::string_view text);
[[nodiscard]] Result<void> validate_asset_references(std::span<const AssetReference> references);
} // namespace dk

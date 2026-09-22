#include <dk/assets/AssetReference.hpp>
#include <unordered_set>

namespace dk {
std::string_view asset_kind_name(AssetKind kind) noexcept
{
    switch (kind) {
    case AssetKind::mesh: return "mesh";
    case AssetKind::material: return "material";
    case AssetKind::texture: return "texture";
    }
    return {};
}
Result<AssetKind> parse_asset_kind(std::string_view text)
{
    for (const auto kind : {AssetKind::mesh, AssetKind::material, AssetKind::texture}) {
        if (asset_kind_name(kind) == text) { return kind; }
    }
    return std::unexpected(Error{ErrorCode::invalid_argument, "Unknown asset kind: " + std::string{text}, {}});
}
Result<void> validate_asset_references(std::span<const AssetReference> references)
{
    if (references.size() > 64) {
        return std::unexpected(Error{ErrorCode::invalid_argument, "At most 64 asset references per entity", {}});
    }
    std::unordered_set<AssetId> seen;
    for (const auto& reference : references) {
        if (reference.id.is_nil() || asset_kind_name(reference.kind).empty() || !seen.insert(reference.id).second) {
            return std::unexpected(Error{ErrorCode::invalid_argument, "Invalid or duplicate asset reference", {reference.id.to_string()}});
        }
    }
    return {};
}
} // namespace dk

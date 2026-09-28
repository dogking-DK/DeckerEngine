#pragma once
#include <dk/assets/AssetReference.hpp>
#include <dk/memory/Containers.hpp>

namespace dk {
inline constexpr std::size_t asset_meta_byte_limit = 2U * 1024U * 1024U;
inline constexpr std::size_t asset_catalog_record_limit = 10000;
struct AssetOutput {
    String key;
    AssetId id;
    AssetKind kind = AssetKind::mesh;
    bool operator==(const AssetOutput&) const = default;
};
struct AssetMetadata {
    AssetId root_id;
    double unit_scale = 1;
    Vector<AssetOutput> outputs;
};
// Construction requires a bound persistent memory resource. Results own that resource.
// ContextError/bad_alloc propagate; format errors use Result and never publish partial values.
[[nodiscard]] Result<AssetMetadata> parse_asset_meta(std::string_view text);
[[nodiscard]] Result<String> serialize_asset_meta(const AssetMetadata& metadata);
} // namespace dk

#pragma once
#include <dk/core/StableId.hpp>
#include <dk/assets/AssetReference.hpp>
#include <dk/math/Transform.hpp>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace dk {
struct EntityData {
    EntityId id;
    std::string name;
    Trsd local;
    std::optional<EntityId> parent;
    std::vector<AssetReference> assets;
};
enum class PropertyType { entity_id, text, vector3, quaternion, optional_entity_id, asset_references };
struct PropertyDescriptor {
    std::string_view name;
    PropertyType type;
    bool read_only = false;
};
struct ComponentDescriptor {
    std::string_view name;
    std::uint32_t version;
    std::span<const PropertyDescriptor> properties;
};
// Stable storage for the lifetime of the process. Names are file-format keys.
[[nodiscard]] std::span<const ComponentDescriptor> scene_component_descriptors() noexcept;
} // namespace dk

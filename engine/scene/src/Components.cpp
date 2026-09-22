#include <dk/scene/Components.hpp>
#include <array>

namespace dk {
namespace {
constexpr std::array identity{PropertyDescriptor{"id", PropertyType::entity_id, true}};
constexpr std::array name{PropertyDescriptor{"value", PropertyType::text}};
constexpr std::array transform{
    PropertyDescriptor{"translation", PropertyType::vector3},
    PropertyDescriptor{"rotation", PropertyType::quaternion},
    PropertyDescriptor{"scale", PropertyType::vector3}};
constexpr std::array hierarchy{PropertyDescriptor{"parent", PropertyType::optional_entity_id}};
constexpr std::array descriptors{
    ComponentDescriptor{"dk.Identity", 1, identity}, ComponentDescriptor{"dk.Name", 1, name},
    ComponentDescriptor{"dk.Transform", 1, transform}, ComponentDescriptor{"dk.Hierarchy", 1, hierarchy}};
} // namespace
std::span<const ComponentDescriptor> scene_component_descriptors() noexcept { return descriptors; }
} // namespace dk

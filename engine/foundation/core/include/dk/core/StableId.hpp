#pragma once

#include <dk/core/Result.hpp>

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <utility>

namespace dk {

namespace detail {

using IdBytes = std::array<std::uint8_t, 16>;

[[nodiscard]] Result<IdBytes> generate_id_bytes();
[[nodiscard]] Result<IdBytes> parse_id_bytes(std::string_view text);
[[nodiscard]] std::string format_id_bytes(const IdBytes& bytes);
[[nodiscard]] std::size_t hash_id_bytes(const IdBytes& bytes) noexcept;

} // namespace detail

template <typename Tag>
class StableId {
public:
    constexpr StableId() noexcept = default;

    [[nodiscard]] static Result<StableId> generate()
    {
        auto bytes = detail::generate_id_bytes();
        if (!bytes) {
            return std::unexpected(std::move(bytes.error()));
        }
        return StableId{*bytes};
    }

    [[nodiscard]] static Result<StableId> parse(std::string_view text)
    {
        auto bytes = detail::parse_id_bytes(text);
        if (!bytes) {
            return std::unexpected(std::move(bytes.error()));
        }
        return StableId{*bytes};
    }

    [[nodiscard]] constexpr bool is_nil() const noexcept
    {
        return bytes_ == detail::IdBytes{};
    }

    [[nodiscard]] std::string to_string() const
    {
        return detail::format_id_bytes(bytes_);
    }

    [[nodiscard]] constexpr const detail::IdBytes& bytes() const noexcept
    {
        return bytes_;
    }

    auto operator<=>(const StableId&) const = default;

private:
    explicit constexpr StableId(detail::IdBytes bytes) noexcept : bytes_{bytes} {}
    detail::IdBytes bytes_{};
};

struct EntityIdTag;
struct AssetIdTag;
struct SceneIdTag;
using EntityId = StableId<EntityIdTag>;
using AssetId = StableId<AssetIdTag>;
using SceneId = StableId<SceneIdTag>;

} // namespace dk

namespace std {

template <typename Tag>
struct hash<dk::StableId<Tag>> {
    [[nodiscard]] size_t operator()(const dk::StableId<Tag>& id) const noexcept
    {
        // A container hash, deliberately not part of the persistence format.
        return dk::detail::hash_id_bytes(id.bytes());
    }
};

} // namespace std

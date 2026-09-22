#include <dk/core/StableId.hpp>

#include <catch2/catch_test_macros.hpp>
#include <uuid.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <thread>
#include <type_traits>
#include <unordered_set>
#include <vector>

namespace {

template <typename Left, typename Right>
concept CanCompare = requires(Left left, Right right) { left == right; };

static_assert(sizeof(dk::EntityId) == 16);
static_assert(std::is_trivially_copyable_v<dk::EntityId>);
static_assert(!std::is_convertible_v<dk::EntityId, dk::AssetId>);
static_assert(!std::is_constructible_v<dk::AssetId, dk::EntityId>);
static_assert(!CanCompare<dk::EntityId, dk::AssetId>);
static_assert(!CanCompare<dk::EntityId, dk::SceneId>);
static_assert(dk::EntityId{}.is_nil());

} // namespace

TEST_CASE("id text preserves bytes and normalizes case", "[id]")
{
    const auto id = dk::EntityId::parse("00112233-4455-4677-8899-AABBCCDDEEFF");
    REQUIRE(id.has_value());
    REQUIRE(id->to_string() == "00112233-4455-4677-8899-aabbccddeeff");
    const std::array<std::uint8_t, 16> bytes{
        0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x46, 0x77,
        0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};
    REQUIRE(id->bytes() == bytes);
    const auto roundtrip = dk::EntityId::parse(id->to_string());
    REQUIRE(roundtrip.has_value());
    REQUIRE(*roundtrip == *id);
}

TEST_CASE("id parser rejects malformed or truncated input", "[id]")
{
    const std::array<std::string_view, 7> invalid{
        "", "00112233-4455-4677-8899-aabbccddeef",
        "00112233-4455-4677-8899-aabbccddeeff0",
        "00112233_4455-4677-8899-aabbccddeeff",
        "00112233-4455-4677-8899-aabbccddeefg",
        " 0112233-4455-4677-8899-aabbccddeeff",
        "{00112233-4455-4677-8899-aabbccddeeff}"};
    for (const auto text : invalid) {
        const auto result = dk::EntityId::parse(text);
        REQUIRE_FALSE(result.has_value());
        REQUIRE(result.error().code == dk::ErrorCode::invalid_argument);
    }
    auto embedded_null = std::string{"00112233-4455-4677-8899-aabbccddeeff"};
    embedded_null[25] = '\0';
    REQUIRE_FALSE(dk::EntityId::parse(embedded_null).has_value());
}

TEST_CASE("nil and existing uuid versions can be persisted", "[id]")
{
    const dk::AssetId nil;
    REQUIRE(nil.is_nil());
    REQUIRE(nil.to_string() == "00000000-0000-0000-0000-000000000000");
    const auto restored = dk::AssetId::parse(nil.to_string());
    REQUIRE(restored.has_value());
    REQUIRE(*restored == nil);
    REQUIRE(dk::SceneId::parse("00112233-4455-7677-8899-aabbccddeeff").has_value());
}

TEST_CASE("id serialization interoperates with stduuid", "[id]")
{
    constexpr std::string_view canonical = "00112233-4455-4677-8899-aabbccddeeff";
    const auto native = uuids::uuid::from_string(canonical);
    REQUIRE(native.has_value());
    const auto id = dk::EntityId::parse(uuids::to_string(*native));
    REQUIRE(id.has_value());
    REQUIRE(uuids::uuid{id->bytes()} == *native);
    REQUIRE(std::hash<dk::EntityId>{}(*id) == std::hash<uuids::uuid>{}(*native));

    // A protocol parser can pass a view into a larger, non-null-terminated field.
    const std::string packet = "prefix" + std::string{canonical} + "suffix";
    const auto field = std::string_view{packet}.substr(6, canonical.size());
    const auto from_field = dk::EntityId::parse(field);
    REQUIRE(from_field.has_value());
    REQUIRE(*from_field == *id);
}

TEST_CASE("id parser keeps canonical policy over stduuid permissive forms", "[id]")
{
    const std::array<std::string_view, 3> permissive{
        "00112233445546778899aabbccddeeff",
        "{00112233-4455-4677-8899-aabbccddeeff}",
        "0011-22334455-4677-8899-aabbccddeeff"};
    for (const auto text : permissive) {
        REQUIRE(uuids::uuid::from_string(text).has_value());
        const auto id = dk::EntityId::parse(text);
        REQUIRE_FALSE(id.has_value());
        REQUIRE(id.error().code == dk::ErrorCode::invalid_argument);
    }
}

TEST_CASE("generated ids have v4 bits and roundtrip through containers", "[id]")
{
    std::unordered_set<dk::AssetId> ids;
    for (int index = 0; index < 1024; ++index) {
        const auto id = dk::AssetId::generate();
        REQUIRE(id.has_value());
        REQUIRE_FALSE(id->is_nil());
        REQUIRE((id->bytes()[6] & 0xf0U) == 0x40U);
        REQUIRE((id->bytes()[8] & 0xc0U) == 0x80U);
        const auto restored = dk::AssetId::parse(id->to_string());
        REQUIRE(restored.has_value());
        REQUIRE(ids.insert(*id).second);
        REQUIRE(ids.contains(*restored));
        REQUIRE(std::hash<dk::AssetId>{}(*id) == std::hash<dk::AssetId>{}(*restored));
    }
}

TEST_CASE("id generation is independent across threads", "[id]")
{
    std::array<std::vector<dk::EntityId>, 4> batches;
    std::atomic<int> failures{0};
    std::vector<std::jthread> workers;
    for (std::size_t batch = 0; batch < batches.size(); ++batch) {
        workers.emplace_back([&, batch] {
            for (int index = 0; index < 128; ++index) {
                auto id = dk::EntityId::generate();
                if (id) {
                    batches[batch].push_back(*id);
                } else {
                    ++failures;
                }
            }
        });
    }
    workers.clear(); // Join before reading the per-thread vectors.
    REQUIRE(failures.load() == 0);
    std::vector<dk::EntityId> ids;
    for (const auto& batch : batches) {
        ids.insert(ids.end(), batch.begin(), batch.end());
    }
    std::sort(ids.begin(), ids.end());
    REQUIRE(ids.size() == 512);
    REQUIRE(std::adjacent_find(ids.begin(), ids.end()) == ids.end());
}

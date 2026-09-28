#include "AssetTestSupport.hpp"
#include "ContentDigest.hpp"
#include <algorithm>

TEST_CASE("content digest matches upstream XXH3 128 canonical vectors", "[assets][digest]")
{
    using namespace dk::asset_detail;
    // xxHash v0.8.4 tests/sanity_test_vectors.h, seed=0; fillTestBuffer from sanity_test.c.
    std::vector<std::byte> data(1578);
    std::uint64_t generator = 2654435761ULL;
    for (auto& value : data) { value = static_cast<std::byte>(generator >> 56); generator *= 11400714785074694797ULL; }
    struct Vector { std::size_t length; std::string_view hex; };
    const Vector vectors[]{{0, "99aa06d3014798d86001c324468d497f"}, {4, "970d585ac632bf8e2e7d8d6876a39fe9"},
        {16, "c68c368ecf8a9c05562980258a998629"}, {32, "98fc6458710dc2e8278410a17595e3f9"},
        {130, "90e701a84d20072fbe0500e135cd4b35"}, {1578, "b04f443d84669721f2e07a2139bbb81f"}};
    for (const auto& vector : vectors) {
        CAPTURE(vector.length);
        const auto bytes = std::span{data}.first(vector.length);
        const auto expected = content_digest(bytes); REQUIRE(expected.hex() == vector.hex);
        ContentHasher streaming; streaming.update({});
        for (std::size_t i = 0; i < bytes.size(); i += 7) { streaming.update(bytes.subspan(i, std::min(std::size_t{7}, bytes.size() - i))); }
        REQUIRE(streaming.digest() == expected); REQUIRE(streaming.digest() == expected);
    }
    const auto empty = content_digest({}); REQUIRE(empty.bytes[0] == std::byte{0x99}); REQUIRE(empty.bytes[15] == std::byte{0x7f});
}
TEST_CASE("content digest streams files across chunk boundaries and reports IO errors", "[assets][digest]")
{
    SceneTestFiles files;
    std::string text(2 * 65536 + 17, 'a'); text[65536] = '\0'; text.back() = 'z';
    files.write("source.bin", text);
    const auto expected = dk::asset_detail::content_digest(std::as_bytes(std::span{text.data(), text.size()}));
    REQUIRE(dk::asset_detail::file_digest(files.root / "source.bin") == expected);
    REQUIRE_FALSE(dk::asset_detail::file_digest(files.root / "missing.bin"));
    files.write("empty.bin", ""); REQUIRE(dk::asset_detail::file_digest(files.root / "empty.bin") == dk::asset_detail::content_digest({}));
}

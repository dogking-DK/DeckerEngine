#include "ContentDigest.hpp"
#include <dk/io/Path.hpp>
#include <dk/profiling/Profiler.hpp>
#include <xxhash.h>
#include <cstring>
#include <fstream>
#include <stdexcept>

namespace dk::asset_detail {
namespace {
ContentDigest canonical(XXH128_hash_t hash)
{
    XXH128_canonical_t value;
    XXH128_canonicalFromHash(&value, hash);
    ContentDigest result;
    std::memcpy(result.bytes.data(), value.digest, result.bytes.size());
    return result;
}
}
std::string ContentDigest::hex() const
{
    constexpr char digits[] = "0123456789abcdef";
    std::string result(32, '0');
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        const auto value = std::to_integer<unsigned>(bytes[i]);
        result[i * 2] = digits[value >> 4]; result[i * 2 + 1] = digits[value & 15];
    }
    return result;
}
ContentDigest content_digest(std::span<const std::byte> bytes)
{ return canonical(XXH3_128bits(bytes.data(), bytes.size())); }
struct ContentHasher::State {
    XXH3_state_t* value = XXH3_createState();
    State() { if (!value) { throw std::bad_alloc{}; } (void)XXH3_128bits_reset(value); }
    ~State() { (void)XXH3_freeState(value); }
};
ContentHasher::ContentHasher() : state_(std::make_unique<State>()) {}
ContentHasher::~ContentHasher() = default;
void ContentHasher::update(std::span<const std::byte> bytes)
{ if (XXH3_128bits_update(state_->value, bytes.data(), bytes.size()) != XXH_OK) { throw std::logic_error("XXH3 update failed"); } }
ContentDigest ContentHasher::digest() const { return canonical(XXH3_128bits_digest(state_->value)); }
Result<ContentDigest> file_digest(const std::filesystem::path& path)
{
    DK_PROFILE_ZONE("Assets.ContentDigest");
    const auto display = path_to_utf8(path);
    if (!display) { return std::unexpected(display.error()); }
    std::ifstream file{path, std::ios::binary};
    if (!file) { return std::unexpected(Error{ErrorCode::io_error, "Cannot open digest input", {*display}}); }
    ContentHasher hasher;
    std::array<std::byte, 64 * 1024> chunk;
    while (file) {
        file.read(reinterpret_cast<char*>(chunk.data()), static_cast<std::streamsize>(chunk.size()));
        hasher.update(std::span{chunk.data(), static_cast<std::size_t>(file.gcount())});
    }
    if (file.bad() || !file.eof()) { return std::unexpected(Error{ErrorCode::io_error, "Cannot read digest input", {*display}}); }
    return hasher.digest();
}
} // namespace dk::asset_detail

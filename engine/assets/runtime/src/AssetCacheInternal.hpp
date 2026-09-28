#pragma once
#include "AssetInternal.hpp"
#include "AssetPersistenceInternal.hpp"
#include "ContentDigest.hpp"
#include <dk/assets/AssetCache.hpp>
#include <dk/io/File.hpp>
#include <functional>

namespace dk::asset_detail {
inline constexpr std::string_view cache_root = ".decker/cache/assets/v1";
inline constexpr std::size_t cache_entry_limit = 16 * 1024 * 1024, cache_index_limit = 16 * 1024;
struct CacheVersions {
    std::string algorithm = "xxh3-128-v1", importer = "gltf-static";
    std::uint64_t cache = 1, artifact = 1, implementation = 1, contract = 1;
    bool operator==(const CacheVersions&) const = default;
};
struct CacheDescriptor {
    CacheVersions versions;
    std::string source;
    AssetMetadata metadata;
    Vector<InputFingerprint> inputs;
};
struct CacheIndex { AssetId root; std::string source, key, build; };
struct CacheEntry { CacheDescriptor descriptor; CpuArtifact artifact; std::string text; };
[[nodiscard]] Result<ContentDigest> cache_key(const CacheDescriptor&);
[[nodiscard]] CacheDescriptor describe_cache(const CpuArtifact&);
[[nodiscard]] nlohmann::json descriptor_json(const CacheDescriptor&);
[[nodiscard]] CacheIndex parse_cache_index(std::string_view);
[[nodiscard]] std::string index_json(const CacheIndex&);
[[nodiscard]] std::string entry_directory(std::string_view key, std::string_view build);
[[nodiscard]] Result<CacheEntry> read_cache_entry(const ProjectPaths&, std::string_view directory,
    std::string_view key, std::string_view build);
[[nodiscard]] std::string read_cache_text(const std::filesystem::path&, std::size_t limit);
[[nodiscard]] std::optional<std::string> optional_cache_text(const std::filesystem::path&, std::size_t limit);
[[nodiscard]] bool cache_path_exists(const std::filesystem::path&);
void write_cache_text(const std::filesystem::path&, std::string_view);
void ordinary_cache_input(const ProjectPaths&, std::string_view);
void verify_cache_inputs(const ProjectPaths&, std::span<const InputFingerprint>);
enum class CacheStep { import, validate_inputs, publish, metadata, current, hit, clean };
using CacheHook = std::function<Result<void>(CacheStep)>;
class ScopedCacheHook {
public:
    explicit ScopedCacheHook(const CacheHook&) noexcept;
    ~ScopedCacheHook();
    ScopedCacheHook(const ScopedCacheHook&) = delete;
    ScopedCacheHook& operator=(const ScopedCacheHook&) = delete;
private:
    const CacheHook* previous_;
};
void cache_step(CacheStep);
} // namespace dk::asset_detail

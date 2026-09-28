#pragma once
#include <dk/assets/Metadata.hpp>
#include <dk/io/Path.hpp>
#include <optional>

namespace dk {
struct CatalogSessionTag;
using CatalogSessionId = StableId<CatalogSessionTag>;
struct CatalogGuard {
    CatalogSessionId session_id;
    std::uint64_t revision = 0;
    bool operator==(const CatalogGuard&) const = default;
};
struct AssetCatalogRecord {
    AssetId id;
    AssetKind kind = AssetKind::mesh;
    String path;
    bool operator==(const AssetCatalogRecord&) const = default;
};
struct AssetOutputSpec { std::string_view key; AssetKind kind = AssetKind::mesh; };
enum class MissingMetaPolicy { reject, create_or_adopt };
struct RegistrationRequest {
    std::string_view source;
    // Empty means mesh/0. Existing mappings are retained, even when not requested here.
    std::span<const AssetOutputSpec> outputs;
    std::optional<double> unit_scale;
    MissingMetaPolicy missing_meta = MissingMetaPolicy::reject;
};

// An immutable preparation result, not proof of a durable registration.
// M4.1.2 will consume this plus the candidate Project and publish only after persistence.
class RegistrationCandidate {
public:
    [[nodiscard]] CatalogGuard base_guard() const noexcept { return base_; }
    [[nodiscard]] CatalogGuard next_guard() const noexcept { return next_; }
    [[nodiscard]] bool changed() const noexcept { return base_ != next_; }
    [[nodiscard]] std::string_view source() const noexcept { return source_; }
    [[nodiscard]] const AssetMetadata& metadata() const noexcept { return metadata_; }
    [[nodiscard]] std::span<const AssetCatalogRecord> records() const noexcept { return records_; }
    [[nodiscard]] const std::optional<String>& expected_meta_bytes() const noexcept { return expected_meta_; }
private:
    friend class AssetCatalog;
    RegistrationCandidate(CatalogGuard base, CatalogGuard next, String source, AssetMetadata metadata,
        Vector<AssetCatalogRecord> records, std::optional<String> expected)
        : base_(base), next_(next), source_(std::move(source)), metadata_(std::move(metadata)),
          records_(std::move(records)), expected_meta_(std::move(expected)) {}
    CatalogGuard base_, next_;
    String source_;
    AssetMetadata metadata_;
    Vector<AssetCatalogRecord> records_;
    std::optional<String> expected_meta_;
};

class AssetCatalog {
public:
    [[nodiscard]] static Result<AssetCatalog> create(const std::filesystem::path& root,
        std::span<const AssetCatalogRecord> records = {});
    AssetCatalog(AssetCatalog&&) noexcept = default;
    AssetCatalog& operator=(AssetCatalog&&) noexcept = default;
    AssetCatalog(const AssetCatalog&) = delete;
    AssetCatalog& operator=(const AssetCatalog&) = delete;
    [[nodiscard]] const ProjectPaths& paths() const noexcept { return paths_; }
    [[nodiscard]] CatalogGuard guard() const noexcept { return guard_; }
    [[nodiscard]] std::span<const AssetCatalogRecord> records() const noexcept { return records_; }
    [[nodiscard]] Result<void> check_guard(CatalogGuard guard) const;
    [[nodiscard]] Result<AssetMetadata> inspect_source(std::string_view source) const;
    [[nodiscard]] Result<RegistrationCandidate> prepare_registration(CatalogGuard guard, const RegistrationRequest& request) const;
    // Read-only preflight: no files, directory revisions, or Project instances are changed.
    [[nodiscard]] Result<void> validate_registration(const RegistrationCandidate& candidate) const;
private:
    AssetCatalog(ProjectPaths paths, Vector<AssetCatalogRecord> records, CatalogGuard guard)
        : paths_(std::move(paths)), records_(std::move(records)), guard_(guard) {}
    ProjectPaths paths_;
    Vector<AssetCatalogRecord> records_;
    CatalogGuard guard_;
};
} // namespace dk

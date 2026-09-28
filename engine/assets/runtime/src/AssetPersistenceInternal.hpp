#pragma once
#include <dk/assets/Persistence.hpp>
#include <dk/assets/Metadata.hpp>
#include <dk/io/Path.hpp>
#include <functional>
#include <optional>

namespace dk::asset_detail {
// Per-thread, scoped seam for deterministic IO failures and process interruption tests.
enum class OperationStep { record, source, meta, manifest, finish,
    rollback_manifest, rollback_meta, rollback_source, rollback_finish };
using OperationHook = std::function<Result<void>(OperationStep)>;
class ScopedOperationHook {
public:
    explicit ScopedOperationHook(const OperationHook& hook) noexcept;
    ~ScopedOperationHook();
    ScopedOperationHook(const ScopedOperationHook&) = delete;
    ScopedOperationHook& operator=(const ScopedOperationHook&) = delete;
private:
    const OperationHook* previous_;
};
[[nodiscard]] bool same_asset_path(const std::filesystem::path& a, const std::filesystem::path& b);
// Validates ordinary names and every existing component, including reparse points.
[[nodiscard]] std::filesystem::path persistent_path(const ProjectPaths& paths, std::string_view relative);
void validate_asset_operation_paths(const ProjectPaths& paths, std::string_view source,
    std::string_view target, std::string_view manifest);
[[nodiscard]] Result<void> commit_asset_files(const ProjectPaths& paths, std::string_view source,
    std::string_view target, const std::optional<String>& before_meta, std::string_view after_meta,
    const AssetManifestUpdate& manifest, bool& needs_recovery);
} // namespace dk::asset_detail

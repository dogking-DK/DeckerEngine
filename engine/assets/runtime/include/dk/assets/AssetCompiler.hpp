#pragma once
#include <dk/assets/GltfImporter.hpp>
namespace dk {
struct AssetCompileRequest {
    std::string_view source, output_directory;
    std::optional<double> unit_scale;
    ImportLimits limits;
};
struct CompiledAsset {
    AssetId root_id;
    String output_directory;
    Vector<OutputIdentity> outputs;
    String summary_json;
};
// Creates a NEW output directory and creates/updates source.meta only after successful import.
// Does not modify Project/Scene. Caller binds an Assets domain and thread scratch.
// Caller serializes writes to the same project; hash rechecks do not replace a filesystem lock.
[[nodiscard]] Result<CompiledAsset> compile_asset(const ProjectPaths& paths, const AssetCompileRequest& request);
} // namespace dk

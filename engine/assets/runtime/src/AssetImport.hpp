#pragma once
#include <dk/assets/GltfImporter.hpp>
namespace dk::asset_detail {
[[nodiscard]] Result<ImportResult> import_asset(const ProjectPaths&,const GltfImportRequest&);
}

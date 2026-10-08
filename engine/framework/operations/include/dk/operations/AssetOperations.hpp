#pragma once
#include <dk/commands/CommandRegistry.hpp>
#include <dk/services/AsyncAssetService.hpp>
namespace dk {
[[nodiscard]] Json catalog_guard_json(CatalogGuard);
[[nodiscard]] Json asset_job_result_schema();
[[nodiscard]] Result<void> register_asset_commands(CommandRegistry&, AsyncAssetService&, bool with_jobs = true);
}

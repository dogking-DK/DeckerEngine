#pragma once
#include <dk/commands/CommandRegistry.hpp>
#include <dk/services/CaptureService.hpp>
namespace dk {
[[nodiscard]] Result<void> register_render_commands(CommandRegistry&,CaptureService&,const SceneService&);
// Current schema subset has no oneOf: optional union properties, with the actual
// complete result variant guaranteed by each service. Asset-only schema stays strict.
[[nodiscard]] Json render_job_result_schema();
}

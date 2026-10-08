#pragma once
#include <dk/commands/CommandRegistry.hpp>
#include <dk/services/SimulationService.hpp>

namespace dk {
[[nodiscard]] Result<void> register_simulation_commands(CommandRegistry&, SimulationService&, const SceneService&);
}

#include <dk/editor/Workspace.hpp>

namespace dk::editor {
bool Workspace::gpu_simulation_available() {
#ifdef DK_SIMULATION_GPU
    return true;
#else
    return false;
#endif
}
Result<void> Workspace::start_simulation(bool paused) {
    if (!snapshot_ || pending())
        return std::unexpected(Error{ErrorCode::invalid_state,"Open a scene and Apply or Revert the Inspector draft before starting."});
    const auto& d=simulation_draft_; const auto& c=d.cloth;
    auto result=call("simulation.run",{{"guard",guard()},{"count",d.target_steps},{"batch_steps",d.batch_steps},
        {"fixed_dt_ns",d.fixed_dt_ns},{"paused",paused},{"solver",d.gpu ? "xpbd_gpu" : "xpbd_cpu"},
        {"cloth",{{"columns",c.columns},{"rows",c.rows},{"seed",c.seed},{"spacing",c.spacing},{"height",c.height},
            {"particle_mass",c.particle_mass},{"compliance",c.compliance},{"iterations",c.physics.iterations},
            {"gravity_y",c.physics.gravity_y},{"floor_y",c.physics.floor_y},{"damping",c.physics.damping}}}});
    if (!result) return std::unexpected(result.error());
    return {};
}
Result<void> Workspace::control_simulation(SimulationId id, SimulationControl control) {
    const char* method=nullptr;
    switch (control) {
    case SimulationControl::pause: method="simulation.pause"; break;
    case SimulationControl::resume: method="simulation.resume"; break;
    case SimulationControl::step: method="simulation.step"; break;
    case SimulationControl::cancel: method="simulation.cancel"; break;
    case SimulationControl::stop: method="simulation.stop"; break;
    }
    if (!method) return std::unexpected(Error{ErrorCode::invalid_argument,"Unknown simulation control"});
    auto result=call(method,{{"run_id",id.to_string()}});
    if (!result) return std::unexpected(result.error());
    return {};
}
}

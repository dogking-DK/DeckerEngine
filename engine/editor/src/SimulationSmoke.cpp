#include "SmokeDriver.hpp"
#include <array>
#include <cstdio>
#include <stdexcept>

namespace dk::editor::detail {
void SmokeDriver::simulation_input(Workspace& model,WorkbenchUi& ui) {
    const auto require=[](bool value,const char* text) { if (!value) throw std::runtime_error(std::string{"Simulation UI smoke: "}+text); };
    auto& io=ImGui::GetIO(); io.ConfigInputTrickleEventQueue=false;
    if (tick_++<20 || done_) return;
    constexpr std::array actions{"sim_prepare","sim_step","sim_resume","sim_pause","sim_cancel","sim_stop","sim_play","sim_stop",
        "sim_solver","sim_gpu","sim_prepare","sim_step","sim_resume","sim_pause","sim_cancel","sim_stop","sim_play","sim_stop"};
    if (!simulation_scene_) {
        simulation_scene_=model.snapshot()->scene; revision_=model.snapshot()->state.revision;
        session_=model.snapshot()->state.document_id; history_=model.history().undo_count;
    }
    const unsigned limit=Workspace::gpu_simulation_available() ? static_cast<unsigned>(actions.size()) : 8;
    if (simulation_stage_>=limit) { done_=true; return; }
    const std::string action=actions[simulation_stage_];
    const auto phase=simulation_phase_++;
    if (phase==0) {
        const auto found=ui.controls().find(action);
        require(found!=ui.controls().end(),("Missing button: "+action).c_str());
        pointer_=found->second;
        io.AddFocusEvent(true); io.AddMousePosEvent(pointer_.x,pointer_.y); io.AddMouseButtonEvent(0,true);
    }
    io.AddMousePosEvent(pointer_.x,pointer_.y);
    if (phase==1) io.AddMouseButtonEvent(0,false);
    if (phase<5) return;
    require(ui.errors()==0,"A control or viewport failed");
    const auto state=model.simulation_state();
    const auto& shown=ui.displayed_simulation();
    require(model.snapshot()->state.document_id==session_ && model.snapshot()->state.revision==revision_ &&
        model.history().undo_count==history_ && model.snapshot()->scene.same_content(*simulation_scene_),"Simulation changed Edit/history");
    if (state.run) require(!state.run->fault,"Simulation faulted");
    bool ready=false;
    if (action=="sim_solver") ready=ui.controls().contains("sim_gpu");
    else if (action=="sim_gpu") ready=model.simulation_draft().gpu;
    else if (action=="sim_stop") ready=!state.run && !shown.run;
    else if (state.run && shown.run && shown.run->run_id==state.run->run_id && state.run->task) {
        const auto status=state.run->task->status;
        if (action=="sim_prepare") {
            ready=status==SimulationTaskStatus::paused && shown.run->task->status==status;
            if (ready) {
                require(state.run->clock.steps==0,"Prepare advanced before Step");
                require(state.run->gpu==model.simulation_draft().gpu,"Wrong solver");
                simulation_id_=state.run->run_id;
            }
        } else if (action=="sim_step") {
            ready=status==SimulationTaskStatus::paused && shown.run->clock.steps==1;
            if (ready) require(state.run->clock.steps==1 && state.run->run_id==simulation_id_,"Step did not preserve run / exact count");
        } else if (action=="sim_resume" || action=="sim_play") ready=status==SimulationTaskStatus::running && state.run->clock.steps>1;
        else if (action=="sim_pause") ready=status==SimulationTaskStatus::paused && shown.run->task->status==status;
        else if (action=="sim_cancel") ready=status==SimulationTaskStatus::cancelled && shown.run->task->status==status;
    }
    if (ready) {
        std::printf("Simulation UI stage %u %s passed\n",simulation_stage_,action.c_str());
        ++simulation_stage_; simulation_phase_=0;
    }
}
}

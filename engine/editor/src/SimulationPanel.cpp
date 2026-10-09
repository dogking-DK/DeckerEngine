#include "WorkbenchUi.hpp"

namespace dk::editor::detail {
void WorkbenchUi::simulation() {
    const bool visible=ImGui::Begin("Simulation");
    displayed_simulation_=model_.simulation_state();
    if (!visible) { ImGui::End(); return; }
    const auto& state=displayed_simulation_;
    const auto* run=state.run ? &*state.run : nullptr;
    const auto* task=run && run->task ? &*run->task : nullptr;
    const bool terminal=task && (task->status==SimulationTaskStatus::succeeded ||
        task->status==SimulationTaskStatus::cancelled || task->status==SimulationTaskStatus::failed);
    const bool stopping=state.mode==SimulationMode::stopping;
    const bool paused=run && !run->fault && state.mode==SimulationMode::paused && !terminal;
    const auto result=[&](const char* label,Result<void> value) {
        simulation_error_=value ? std::string{} : value.error().message;
        report(label,std::move(value));
    };
    const auto control=[&](const char* label,const char* id,SimulationControl action,bool enabled) {
        ImGui::BeginDisabled(!enabled);
        if (ImGui::Button(label)) result(label,model_.control_simulation(run->run_id,action));
        mark(id); ImGui::EndDisabled();
    };
    ImGui::BeginDisabled(input_.active());
    ImGui::BeginDisabled(run || !model_.snapshot() || model_.pending());
    if (ImGui::Button("Play")) result("Simulation Play",model_.start_simulation()); mark("sim_play");
    ImGui::SameLine();
    if (ImGui::Button("Prepare paused")) result("Simulation prepare",model_.start_simulation(true)); mark("sim_prepare");
    ImGui::EndDisabled();
    control("Pause","sim_pause",SimulationControl::pause,run && !run->fault && !terminal &&
        (state.mode==SimulationMode::running || state.mode==SimulationMode::initializing));
    ImGui::SameLine(); control("Resume","sim_resume",SimulationControl::resume,paused);
    ImGui::SameLine(); control("Step","sim_step",SimulationControl::step,paused);
    control("Cancel","sim_cancel",SimulationControl::cancel,task && !terminal && !stopping && state.mode!=SimulationMode::cancelling);
    ImGui::SameLine(); control("Stop","sim_stop",SimulationControl::stop,run && !stopping);
    ImGui::EndDisabled();
    if (!simulation_error_.empty()) ImGui::TextWrapped("Control failed: %s",simulation_error_.c_str());
    if (model_.pending() && !run) ImGui::TextWrapped("Apply or Revert the Inspector draft before starting.");
    if (run) {
        const auto status=task ? simulation_task_status_name(task->status) :
            state.mode==SimulationMode::paused ? std::string_view{"paused"} : std::string_view{"running"};
        ImGui::Text("State: %.*s  |  %s",static_cast<int>(status.size()),status.data(),run->gpu ? "GPU XPBD" : run->cloth ? "CPU XPBD" : "Clock only");
        ImGui::Text("Completed: %llu",static_cast<unsigned long long>(run->clock.steps));
        if (task) ImGui::Text("Target: %llu  |  In flight: %llu",static_cast<unsigned long long>(task->target_steps),
            static_cast<unsigned long long>(task->submitted_steps-task->completed_steps));
        if (run->fault) ImGui::TextWrapped("Simulation failed: %.*s. Stop before restarting.",
            static_cast<int>(error_code_name(*run->fault).size()),error_code_name(*run->fault).data());
        if (ImGui::TreeNode("Active configuration")) {
            ImGui::TextWrapped("Run: %s",run->run_id.to_string().c_str());
            ImGui::Text("dt: %.6f ms",static_cast<double>(run->config.fixed_dt_ns)/1000000.0);
            if (run->cloth) {
                const auto& c=*run->cloth;
                ImGui::Text("%u x %u  |  seed %u  |  %u iterations",c.columns,c.rows,c.seed,c.physics.iterations);
                ImGui::Text("Spacing %.6g  Height %.6g  Mass %.6g",c.spacing,c.height,c.particle_mass);
                ImGui::Text("Compliance %.6g  Damping %.6g",c.compliance,c.physics.damping);
                ImGui::Text("Gravity %.6g  Floor %.6g",c.physics.gravity_y,c.physics.floor_y);
            }
            ImGui::TreePop();
        }
    } else ImGui::TextUnformatted("State: edit (no active simulation)");
    ImGui::TextWrapped("Viewport shows the editing scene. Live cloth preview is not available yet.");
    if (ImGui::CollapsingHeader("Next experiment",ImGuiTreeNodeFlags_DefaultOpen)) {
        auto& d=model_.simulation_draft(); auto& c=d.cloth;
        ImGui::TextWrapped("Changes apply when starting a new experiment.");
        ImGui::SetNextItemWidth(140);
        const bool choose=ImGui::BeginCombo("Solver",d.gpu ? "GPU XPBD" : "CPU XPBD");
        mark("sim_solver");
        if (choose) {
            if (ImGui::Selectable("CPU XPBD",!d.gpu)) d.gpu=false;
            mark("sim_cpu");
            if (Workspace::gpu_simulation_available()) {
                if (ImGui::Selectable("GPU XPBD",d.gpu)) d.gpu=true;
                mark("sim_gpu");
            }
            ImGui::EndCombo();
        }
        if (ImGui::BeginTable("parameters",2,ImGuiTableFlags_SizingStretchSame)) {
            const auto field=[](const char* label,ImGuiDataType type,void* value) {
                ImGui::PushID(label); ImGui::TableNextRow(); ImGui::TableNextColumn();
                ImGui::TextWrapped("%s",label); ImGui::TableNextColumn(); ImGui::SetNextItemWidth(-1);
                ImGui::InputScalar("##value",type,value,nullptr,nullptr,type==ImGuiDataType_Float ? "%.6g" : nullptr);
                ImGui::PopID();
            };
            field("Target steps",ImGuiDataType_S32,&d.target_steps); mark("sim_target");
            field("Batch steps (1-8)",ImGuiDataType_S32,&d.batch_steps);
            field("Fixed dt (ns)",ImGuiDataType_S64,&d.fixed_dt_ns);
            field("Columns",ImGuiDataType_U32,&c.columns);
            field("Rows",ImGuiDataType_U32,&c.rows);
            field("Seed",ImGuiDataType_U32,&c.seed);
            field("Spacing (m)",ImGuiDataType_Float,&c.spacing);
            field("Height (m)",ImGuiDataType_Float,&c.height);
            field("Mass (kg)",ImGuiDataType_Float,&c.particle_mass);
            field("Compliance (m/N)",ImGuiDataType_Float,&c.compliance);
            field("Iterations",ImGuiDataType_U32,&c.physics.iterations);
            field("Gravity (m/s2)",ImGuiDataType_Float,&c.physics.gravity_y);
            field("Floor (m)",ImGuiDataType_Float,&c.physics.floor_y);
            field("Damping (1/s)",ImGuiDataType_Float,&c.physics.damping);
            ImGui::EndTable();
        }
        if (ImGui::Button("Reset draft")) d=SimulationDraft{};
    }
    ImGui::End();
}
}

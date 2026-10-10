#include "WorkbenchUi.hpp"
#include <imgui_stdlib.h>

namespace dk::editor::detail {
void WorkbenchUi::simulation_results(const SimulationRunState* run) {
    const bool settled=run && run->cloth && !run->fault && displayed_simulation_.mode==SimulationMode::paused &&
        (!run->task || run->task->initialized);
    ImGui::BeginDisabled(!settled || input_.active());
    if (ImGui::Button("Sample metrics")) {
        auto result=model_.sample_simulation(run->run_id,run->clock.steps);
        simulation_sample_error_=result ? std::string{} : result.error().message;
        report("Sample metrics",std::move(result));
    }
    mark("sim_sample");
    if (Workspace::gpu_simulation_available()) {
        ImGui::SameLine();
        if (ImGui::Button("Export")) {
            const auto output=simulation_output_.empty() ? "experiment-"+run->run_id.to_string()+"-"+std::to_string(run->clock.steps) : simulation_output_;
            report("Export experiment",model_.export_simulation(run->run_id,run->clock.steps,output,simulation_export_width_,simulation_export_height_));
        }
        mark("sim_export");
    }
    ImGui::EndDisabled();
    if (!settled) ImGui::TextWrapped("Pause at a healthy completed step to sample or export.");
    if (!simulation_sample_error_.empty()) ImGui::TextWrapped("Sampling failed: %s",simulation_sample_error_.c_str());
    if (!model_.simulation_export_error().empty()) ImGui::TextWrapped("Last export failed: %s",model_.simulation_export_error().c_str());

    const auto current=[&](std::uint64_t steps) { return run && !run->fault && displayed_simulation_.mode!=SimulationMode::stopping && steps==run->clock.steps; };
    const auto* performance=run && run->task && run->task->performance ? &*run->task->performance : nullptr;
    if (performance) {
        ImGui::Text("Performance: %s, step %llu",current(performance->steps) ? "Current" : "Stale",static_cast<unsigned long long>(performance->steps));
        const double ms=static_cast<double>(performance->wall_time_ns)/1000000.0;
        ImGui::Text("Batch: %.3f ms / %u steps",ms,performance->batch_steps);
        ImGui::Text("Mean: %.3f ms / step",ms/static_cast<double>(performance->batch_steps));
    } else ImGui::TextWrapped("Performance: Unavailable (no completed finite batch)");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Wall time from submit to completion, including CPU work and GPU wait/poll.\nExcludes initialization, preview and diagnostics; not GPU timestamp time.");

    const auto* sample=run && run->metrics_sample ? &*run->metrics_sample : nullptr;
    if (sample) {
        ImGui::Text("Numerics: %s, step %llu",current(sample->steps) ? "Current" : "Stale",static_cast<unsigned long long>(sample->steps));
        ImGui::Text("Sample time: %.6f s",static_cast<double>(sample->simulated_time_ns)/1000000000.0);
        const auto& m=sample->metrics;
        ImGui::TextWrapped("Constraint max / RMS: %.5g / %.5g m",m.max_constraint_error,m.rms_constraint_error);
        if (ImGui::TreeNode("All numerical metrics")) {
            ImGui::Text("Particles %u / constraints %u / colors %u",m.particle_count,m.constraint_count,m.color_count);
            if (ImGui::BeginTable("metrics",2,ImGuiTableFlags_SizingStretchSame)) {
                const auto metric=[](const char* label,double value) {
                    ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextWrapped("%s",label);
                    ImGui::TableNextColumn(); ImGui::Text("%.7g",value);
                };
                metric("Relative error",m.max_relative_error); metric("Max speed (m/s)",m.max_speed);
                metric("Kinetic energy (J)",m.kinetic_energy); metric("Gravity potential (J)",m.gravity_potential_energy);
                metric("Compliant energy (J)",m.compliant_energy); metric("Min height (m)",m.min_height);
                metric("Penetration (m)",m.max_penetration); metric("Pin displacement (m)",m.max_pin_displacement);
                ImGui::EndTable();
            }
            ImGui::TreePop();
        }
    } else ImGui::TextWrapped("Numerics: Unavailable (not sampled)");

    if (Workspace::gpu_simulation_available() && ImGui::TreeNode("Export options")) {
        ImGui::TextWrapped("New project-relative directory. Leave blank for an automatic run/step name.");
        ImGui::SetNextItemWidth(-1); ImGui::InputText("##experiment_output",&simulation_output_);
        ImGui::SetNextItemWidth(100); ImGui::InputInt("Width (1-2048)",&simulation_export_width_);
        ImGui::SetNextItemWidth(100); ImGui::InputInt("Height (1-2048)",&simulation_export_height_);
        ImGui::TextWrapped("Exports config.json, metrics.json, particles.json, image.ppm and provenance.json using the fixed experiment camera.");
        ImGui::TextWrapped("Sampling and export explicitly wait for GPU readback; export also writes files.");
        ImGui::TreePop();
    }
    if (const auto& receipt=model_.simulation_export()) {
        ImGui::Separator(); ImGui::TextUnformatted("Last successful export");
        ImGui::TextWrapped("%s",receipt->output.c_str());
        ImGui::TextWrapped("Run: %s",receipt->run_id.to_string().c_str());
        ImGui::Text("Step %llu | %.6f s",static_cast<unsigned long long>(receipt->steps),static_cast<double>(receipt->simulated_time_ns)/1000000000.0);
    }
}
}

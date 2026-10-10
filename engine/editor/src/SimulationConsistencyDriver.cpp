#include "SimulationConsistencyDriver.hpp"
#include "EditorSupport.hpp"
#include <dk/io/File.hpp>
#include <array>
#include <cstdio>

namespace dk::editor::detail {
namespace {
constexpr std::array names{"initial","prepared","stepped","sampled","advanced","exported","running","paused","stopped","replayed"};
void require(bool ok,const char* message) {
    if (!ok) throw std::runtime_error(std::string{"Simulation consistency: "}+message);
}
std::span<const char* const> actions(unsigned stage,bool gpu) {
    static constexpr std::array cpu_prepare{"sim_prepare"};
    static constexpr std::array gpu_prepare{"sim_solver","sim_gpu","sim_prepare"};
    static constexpr std::array step{"sim_step"},sample{"sim_sample"};
    static constexpr std::array exported{"sim_sample","sim_export"};
    static constexpr std::array resume{"sim_resume"},stop{"sim_stop"};
    switch (stage) {
    case 1: if (gpu) return gpu_prepare; else return cpu_prepare;
    case 2: return step;
    case 3: return sample;
    case 5: return exported;
    case 6: return resume;
    case 8: return stop;
    default: return {};
    }
}
}
SimulationConsistencyDriver::SimulationConsistencyDriver(const std::filesystem::path& root)
    : directory_(root/".dk-simulation-consistency") {
    require(std::filesystem::create_directory(directory_),"checkpoint directory must be fresh");
}
std::string SimulationConsistencyDriver::checkpoint() const { return std::string{backend_ ? "gpu-" : "cpu-"}+names[stage_]; }
void SimulationConsistencyDriver::input(WorkbenchUi& ui) {
    if (done_) return;
    if (waiting_) {
        if (!std::filesystem::is_regular_file(directory_/(checkpoint()+".continue"))) return;
        waiting_=false; ready_=false; tick_=0; stable_frames_=0;
        if (++stage_==names.size()) {
            if (++backend_==2) { done_=true; return; }
            stage_=0; initial_scene_.reset(); edited_scene_.reset(); run_id_.reset();
        }
    }
    auto& io=ImGui::GetIO(); io.ConfigInputTrickleEventQueue=false;
    if (tick_++<12) return;
    const auto sequence=actions(stage_,backend_!=0);
    const auto elapsed=tick_-13,step=elapsed/8,phase=elapsed%8;
    if (step>=sequence.size()) { ready_=true; return; }
    if (phase==0) {
        io.AddFocusEvent(true);
        const auto control=ui.controls().find(sequence[step]);
        require(control!=ui.controls().end(),"missing GUI control");
        pointer_=control->second; io.AddMousePosEvent(pointer_.x,pointer_.y); io.AddMouseButtonEvent(0,true);
    }
    io.AddMousePosEvent(pointer_.x,pointer_.y);
    if (phase==1) io.AddMouseButtonEvent(0,false);
}
void SimulationConsistencyDriver::presented(const Workspace& model,const WorkbenchUi& ui,Viewport& viewport) {
    if (done_ || waiting_ || !ready_) return;
    require(ui.errors()==0 && !ui.closing() && viewport.error().empty(),"unexpected UI error");
    const auto& edit=*model.snapshot();
    const auto state=model.simulation_state(); const auto& shown=ui.displayed_simulation();
    const auto* run=state.run ? &*state.run : nullptr;
    const bool edit_stage=stage_==0 || stage_>=8;
    if (edit_stage) {
        if (run || shown.run || viewport.simulation_run() || !viewport.current(edit.state)) return;
    } else {
        if (!run || !shown.run || !run->task || !run->task->initialized) return;
        require(!run->fault && !shown.run->fault,"simulation faulted");
        require(run->gpu==(backend_!=0),"wrong simulation backend");
        require(shown.run->run_id==run->run_id && shown.run->clock==run->clock && shown.mode==state.mode,
            "GUI does not show owner-published state");
        if (viewport.simulation_run()!=run->run_id || !viewport.texture() || !viewport.simulation_view_current()) return;
        if (stage_==6) {
            if (state.mode!=SimulationMode::running || run->clock.steps<=300) return;
            require(viewport.simulation_steps()<=run->clock.steps,"preview claims a future step");
        } else {
            if (state.mode!=SimulationMode::paused || viewport.simulation_steps()!=run->clock.steps) return;
            const auto expected=stage_==1 ? 0u : stage_<=3 ? 1u : 300u;
            if (stage_!=7) require(run->clock.steps==expected,"wrong exact completed count");
        }
        if (stage_==1) {
            require(!run->metrics_sample,"new run inherited a sample"); run_id_=run->run_id;
        } else require(run_id_==run->run_id,"GUI/IPC switched active experiment");
        require(run->source.document_id==document_id_ && run->source.revision==initial_revision_,"Play source changed with Edit");
        require(run->clock.simulated_time_ns==static_cast<std::int64_t>(run->clock.steps)*run->config.fixed_dt_ns,"clock differs from exact steps");
        if (stage_>=3) {
            require(run->metrics_sample && shown.run->metrics_sample,"missing explicit sample");
            const auto sample_steps=stage_<=4 ? 1u : 300u;
            require(run->metrics_sample->steps==sample_steps && shown.run->metrics_sample->steps==sample_steps &&
                shown.run->metrics_sample->metrics==run->metrics_sample->metrics,"GUI sample differs from the service or lost stale identity");
        }
    }
    if (stage_==0) {
        initial_scene_=edit.scene; document_id_=edit.state.document_id;
        initial_revision_=edit.state.revision; initial_history_=model.history().undo_count;
    } else {
        require(edit.state.document_id==document_id_,"Edit session changed during simulation");
        if (stage_<4) require(edit.scene.same_content(*initial_scene_) && edit.state.revision==initial_revision_ &&
            model.history().undo_count==initial_history_,"GUI simulation modified Edit/history");
        else {
            if (stage_==4) {
                require(edit.state.revision==initial_revision_+1 && !edit.scene.same_content(*initial_scene_),"external edit did not reach GUI");
                edited_scene_=edit.scene;
            }
            require(edit.scene.same_content(*edited_scene_) && model.history().undo_count==initial_history_+1 &&
                model.history().redo_count==0,"latest Edit/history was lost during Stop or replay");
        }
    }
    const auto pixels=edit_stage ? viewport.pixel_signature() : viewport.capture_simulation_pixels();
    if (stage_==7) {
        if (!stable_frames_) { pause_steps_=run->clock.steps; pause_pixels_=pixels; }
        require(run->clock.steps==pause_steps_ && pixels==pause_pixels_,"paused clock/image drifted between presented frames");
        if (++stable_frames_<8) return;
    }
    const auto& receipt=model.simulation_export();
    if (stage_==5) require(receipt && receipt->run_id==run->run_id && receipt->steps==300 &&
        receipt->simulated_time_ns==3000000000 && model.simulation_export_error().empty(),"wrong GUI export receipt");
    Json names_json=Json::object(); for (const auto& entity:edit.scene.entities()) names_json[entity.id.to_string()]=entity.name;
    Json simulation=nullptr;
    if (run) simulation={{"run_id",run->run_id.to_string()},{"gpu",run->gpu},{"status",simulation_task_status_name(run->task->status)},
        {"steps",run->clock.steps},{"simulated_time_ns",run->clock.simulated_time_ns},{"source_revision",run->source.revision},
        {"sample_steps",run->metrics_sample ? Json(run->metrics_sample->steps) : Json(nullptr)}};
    const auto name=checkpoint();
    Json report{{"checkpoint",name},{"simulation",std::move(simulation)},
        {"edit",{{"document_id",edit.state.document_id.to_string()},{"revision",edit.state.revision},{"dirty",edit.state.dirty},
            {"names",std::move(names_json)},{"undo_count",model.history().undo_count},{"redo_count",model.history().redo_count}}},
        {"viewport",{{"kind",edit_stage ? "edit" : "simulation"},{"steps",edit_stage ? Json(nullptr) : Json(viewport.simulation_steps())},
            {"rgba_signature",pixels},{"width",viewport.info().width},{"height",viewport.info().height}}},
        {"export",receipt ? Json{{"run_id",receipt->run_id.to_string()},{"steps",receipt->steps},{"output",receipt->output}} : Json(nullptr)}};
    const auto text=report.dump(2);
    check(write_file_bytes_atomic(directory_/(name+".json"),std::as_bytes(std::span{text.data(),text.size()})));
    waiting_=true;
    std::printf("Simulation consistency checkpoint %s\n",name.c_str()); std::fflush(stdout);
}
}

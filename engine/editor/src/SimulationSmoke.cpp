#include "SmokeDriver.hpp"
#include "EditorSupport.hpp"
#include <array>
#include <cstdio>

namespace dk::editor::detail {
void SmokeDriver::poll_window(platform::Window& window,bool minimized) {
    if (restore_at_==std::chrono::steady_clock::time_point{}) return;
    if (minimized) ++suspended_frames_;
    if (std::chrono::steady_clock::now()>=restore_at_) { check(window.restore()); restore_at_={}; }
}
void SmokeDriver::simulation_input(Workspace& model,WorkbenchUi& ui,platform::Window& window,Viewport& viewport) {
    const auto require=[](bool value,const char* text) { if (!value) throw std::runtime_error(std::string{"Simulation UI smoke: "}+text); };
    auto& io=ImGui::GetIO(); io.ConfigInputTrickleEventQueue=false;
    if (tick_++<20 || done_) return;
    constexpr std::array actions{"sim_prepare",":initial","sim_step",":stepped","sim_resume",":running","sim_pause",":paused",
        ":orbit",":pan",":zoom","simulation_frame",":resize",":minimize","simulation_edit_view","simulation_view",
        "sim_cancel","sim_stop","sim_play",":running","sim_stop"};
    const bool select_gpu=simulation_backend_==1 && simulation_stage_<2;
    const unsigned offset=simulation_backend_==1 ? 2 : 0;
    std::string action;
    if (select_gpu) action=simulation_stage_==0 ? "sim_solver" : "sim_gpu";
    else if (simulation_stage_<actions.size()+offset) action=actions[simulation_stage_-offset];
    else if (simulation_backend_==0 && Workspace::gpu_simulation_available()) { simulation_backend_=1; simulation_stage_=0; return; }
    else {
        // Close with a live preview and task, after also exercising Stop/restart above.
        const auto extra=simulation_stage_-static_cast<unsigned>(actions.size())-offset;
        if (extra==0) action="sim_play";
        else if (extra==1) action=":running";
        else { done_=true; return; }
    }
    if (!simulation_scene_) {
        simulation_scene_=model.snapshot()->scene; revision_=model.snapshot()->state.revision;
        session_=model.snapshot()->state.document_id; history_=model.history().undo_count;
    }
    const auto phase=simulation_phase_++;
    const bool navigation=action==":orbit" || action==":pan";
    const int button=action==":orbit" ? 1 : action==":pan" ? 2 : 0;
    const auto control=[&](const std::string& name) {
        const auto found=ui.controls().find(name); require(found!=ui.controls().end(),("Missing control: "+name).c_str()); return found->second;
    };
    if (phase==0) {
        simulation_stable_=0; simulation_observed_step_=0;
        simulation_camera_revision_=ui.simulation_camera().revision();
        if (viewport.simulation_run() && viewport.texture()) stage_pixels_=viewport.capture_simulation_pixels();
        io.AddFocusEvent(true);
        if (action==":resize") check(window.resize(simulation_backend_ ? 1360 : 1280,simulation_backend_ ? 820 : 800));
        else if (action==":minimize") {
            suspended_frames_=0; check(window.minimize()); restore_at_=std::chrono::steady_clock::now()+std::chrono::milliseconds{150};
        } else if (navigation || action==":zoom") {
            pointer_=control("simulation_viewport"); start_pointer_=pointer_;
            io.AddMousePosEvent(pointer_.x,pointer_.y);
            if (navigation) io.AddMouseButtonEvent(button,true); else io.AddMouseWheelEvent(0,2);
        } else if (!action.starts_with(":")) {
            pointer_=control(action); io.AddMousePosEvent(pointer_.x,pointer_.y); io.AddMouseButtonEvent(0,true);
        }
    }
    if (navigation && phase==2) pointer_={start_pointer_.x+36,start_pointer_.y+18};
    io.AddMousePosEvent(pointer_.x,pointer_.y);
    if (phase==1 && !action.starts_with(":")) io.AddMouseButtonEvent(0,false);
    if (navigation && phase==3) io.AddMouseButtonEvent(button,false);
    if (phase<6) return;
    require(ui.errors()==0,"A control or edit viewport failed");
    const auto state=model.simulation_state(); const auto& shown=ui.displayed_simulation();
    require(model.snapshot()->state.document_id==session_ && model.snapshot()->state.revision==revision_ &&
        model.history().undo_count==history_ && model.snapshot()->scene.same_content(*simulation_scene_),"Simulation changed Edit/history");
    if (state.run) require(!state.run->fault,"Simulation faulted");
    require(viewport.error().empty(),viewport.error().c_str());
    const bool image=state.run && viewport.simulation_run()==state.run->run_id && viewport.texture() && viewport.simulation_view_current();
    bool ready=false;
    if (action=="sim_solver") ready=ui.controls().contains("sim_gpu");
    else if (action=="sim_gpu") ready=model.simulation_draft().gpu;
    else if (action=="sim_stop") ready=!state.run && !shown.run && !viewport.simulation_run() && viewport.current(model.snapshot()->state);
    else if (action=="simulation_edit_view") ready=!viewport.simulation_run() && viewport.current(model.snapshot()->state);
    else if (state.run && shown.run && shown.run->run_id==state.run->run_id && state.run->task) {
        const auto status=state.run->task->status;
        if (action=="sim_prepare") {
            ready=status==SimulationTaskStatus::paused && shown.run->task->status==status && image;
            if (ready) {
                require(state.run->clock.steps==0 && viewport.simulation_steps()==0,"Prepare advanced before Step");
                require(state.run->gpu==model.simulation_draft().gpu,"Wrong solver"); simulation_id_=state.run->run_id;
            }
        } else if (action=="sim_step") {
            ready=status==SimulationTaskStatus::paused && shown.run->clock.steps==1 && image && viewport.simulation_steps()==1;
            if (ready) require(state.run->clock.steps==1 && state.run->run_id==simulation_id_,"Step did not preserve run / exact count");
        } else if (action=="sim_resume" || action=="sim_play") ready=status==SimulationTaskStatus::running && state.run->clock.steps>1 && image;
        else if (action=="sim_pause") ready=status==SimulationTaskStatus::paused && image && viewport.simulation_steps()==state.run->clock.steps;
        else if (action=="sim_cancel") ready=status==SimulationTaskStatus::cancelled && shown.run->task->status==status;
        else if (action==":initial") {
            ready=image && viewport.simulation_steps()==0;
            if (ready) { pixels_=viewport.capture_simulation_pixels(); require(!viewport.retained_pixels().empty(),"No cloth pixels"); }
        } else if (action==":stepped") ready=image && viewport.simulation_steps()==1;
        else if (action==":running") {
            if (image && viewport.simulation_steps()>1) {
                if (!simulation_observed_step_) simulation_observed_step_=viewport.simulation_steps();
                else ready=viewport.simulation_steps()>simulation_observed_step_;
                if (ready) require(viewport.capture_simulation_pixels()!=pixels_,"Running cloth did not change initial pixels");
            }
        } else if (action==":paused") {
            if (image && viewport.simulation_steps()==state.run->clock.steps) {
                const auto hash=viewport.capture_simulation_pixels();
                if (!simulation_stable_) { camera_pixels_=hash; simulation_observed_step_=state.run->clock.steps; }
                require(hash==camera_pixels_ && state.run->clock.steps==simulation_observed_step_,"Paused image or clock changed");
                ready=++simulation_stable_>=8;
            }
        } else if (navigation || action==":zoom" || action=="simulation_frame") {
            ready=image && ui.simulation_camera().revision()!=simulation_camera_revision_;
            if (ready) require(viewport.capture_simulation_pixels()!=stage_pixels_,"Camera input did not alter cloth image");
        } else if (action==":resize") ready=image && viewport.capture_simulation_pixels()!=stage_pixels_;
        else if (action==":minimize") { ready=image && restore_at_==std::chrono::steady_clock::time_point{}; if (ready) require(suspended_frames_>0,"Minimized loop not exercised"); }
        else if (action=="simulation_view") ready=image && viewport.simulation_steps()==state.run->clock.steps;
    }
    if (ready) {
        std::printf("Simulation viewport backend=%s stage=%u %s passed\n",simulation_backend_ ? "GPU" : "CPU",simulation_stage_,action.c_str());
        ++simulation_stage_; simulation_phase_=0;
    }
}
}

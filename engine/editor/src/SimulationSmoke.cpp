#include "SmokeDriver.hpp"
#include "EditorSupport.hpp"
#include <array>
#include <SDL3/SDL.h>
#include <cmath>
#include <cstdio>

namespace dk::editor::detail {
void SmokeDriver::poll_window(platform::Window& window,bool minimized) {
    navigation_smoke_.poll(window,minimized);
    if (restore_at_==std::chrono::steady_clock::time_point{}) return;
    if (minimized) ++suspended_frames_;
    if (std::chrono::steady_clock::now()>=restore_at_) { check(window.restore()); restore_at_={}; }
}
void SmokeDriver::simulation_input(Workspace& model,WorkbenchUi& ui,platform::Window& window,Viewport& viewport) {
    const auto require=[](bool value,const char* text) { if (!value) throw std::runtime_error(std::string{"Simulation UI smoke: "}+text); };
    auto& io=ImGui::GetIO(); io.ConfigInputTrickleEventQueue=false;
    if (tick_++<20 || done_) return;
    constexpr std::array actions{"sim_prepare",":initial","sim_sample","sim_step",":stepped","sim_sample","sim_export","sim_export_duplicate","sim_resume",":running",":orbit_running",":pan_running","sim_pause",":paused",
        "simulation_camera_settings","simulation_camera_fov",":all",":fov",":enter","simulation_camera_speed",":all",":nav_speed",":enter","simulation_camera_apply",
        "simulation_camera_fly",":fly","simulation_camera_walk",":walk","simulation_camera_fly",":switch_capture","simulation_view","simulation_camera_orbit",
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
    const bool orbit=action==":orbit" || action==":orbit_running";
    const bool switch_capture=action==":switch_capture";
    const bool free=action==":fly" || action==":walk";
    const bool navigation=free || orbit || action==":pan" || action==":pan_running";
    const int button=(orbit || free || switch_capture) ? 1 : navigation ? 2 : 0;
    const auto control=[&](const std::string& name) {
        const auto found=ui.controls().find(name); require(found!=ui.controls().end(),("Missing control: "+name).c_str()); return found->second;
    };
    const auto export_bytes=[&] {
        std::vector<ByteBuffer> files;
        const auto& receipt=model.simulation_export(); require(receipt.has_value(),"Missing export receipt");
        const auto directory=model.snapshot()->project.paths().root()/take(path_from_utf8(receipt->output));
        for (const auto* name : {"config.json","metrics.json","particles.json","image.ppm","provenance.json"})
            files.push_back(take(read_file_bytes(directory/name)));
        return files;
    };
    if (phase==0) {
        simulation_stable_=0; simulation_observed_step_=0;
        simulation_camera_revision_=ui.simulation_camera().revision();
        navigation_pose_=ui.simulation_camera().world().matrix(); edit_pose_=ui.input().camera().world().matrix();
        if (viewport.simulation_run() && viewport.texture()) stage_pixels_=viewport.capture_simulation_pixels();
        io.AddFocusEvent(true);
        if (action=="sim_export_duplicate") { ++simulation_expected_errors_; simulation_export_bytes_=export_bytes(); }
        if(action==":all") { io.AddKeyEvent(ImGuiMod_Ctrl,true); io.AddKeyEvent(ImGuiKey_A,true); }
        else if(action==":nav_speed") io.AddInputCharactersUTF8("0.1");
        else if(action==":fov") io.AddInputCharactersUTF8(simulation_backend_ ? "75" : "55");
        else if(action==":enter") io.AddKeyEvent(ImGuiKey_Enter,true);
        else if (action==":resize") check(window.resize(simulation_backend_ ? 1360 : 1280,simulation_backend_ ? 820 : 800));
        else if (action==":minimize") {
            suspended_frames_=0; check(window.minimize()); restore_at_=std::chrono::steady_clock::now()+std::chrono::milliseconds{150};
        } else if (navigation || action==":zoom" || switch_capture) {
            pointer_=control("simulation_viewport"); start_pointer_=pointer_;
            io.AddMousePosEvent(pointer_.x,pointer_.y);
            if (navigation || switch_capture) io.AddMouseButtonEvent(button,true); else io.AddMouseWheelEvent(0,2);
            if (free || switch_capture) { io.AddKeyEvent(ImGuiKey_W,true); if(free) io.AddKeyEvent(ImGuiKey_E,true); }
            if (navigation) {
                const auto now=std::chrono::steady_clock::now();
                navigation_probe_={now,now,viewport.simulation_view_sequence()};
                simulation_observed_step_=model.simulation_state().run->clock.steps;
            }
        } else if (!action.starts_with(":")) {
            pointer_=control(action=="sim_export_duplicate" ? "sim_export" : action); io.AddMousePosEvent(pointer_.x,pointer_.y); io.AddMouseButtonEvent(0,true);
        }
    }
    if (navigation && !navigation_probe_.finished) {
        const auto now=std::chrono::steady_clock::now();
        const auto elapsed=std::chrono::duration<double,std::milli>(now-navigation_probe_.start).count();
        const auto gap=std::chrono::duration<double,std::milli>(now-navigation_probe_.last_update).count();
        if (viewport.simulation_view_sequence()!=navigation_probe_.sequence) {
            navigation_probe_.max_gap_ms=std::max(navigation_probe_.max_gap_ms,gap);
            navigation_probe_.last_update=now; navigation_probe_.sequence=viewport.simulation_view_sequence();
            ++navigation_probe_.updates;
        }
        if (elapsed>=1000) {
            navigation_probe_.max_gap_ms=std::max(navigation_probe_.max_gap_ms,
                std::chrono::duration<double,std::milli>(now-navigation_probe_.last_update).count());
            std::printf("Simulation navigation backend=%s action=%s duration_ms=%.1f updates=%u max_gap_ms=%.1f\n",
                simulation_backend_ ? "GPU" : "CPU",action.c_str(),elapsed,navigation_probe_.updates,navigation_probe_.max_gap_ms);
            require(navigation_probe_.updates>=5 && navigation_probe_.max_gap_ms<400,"Continuous camera input starved completed images");
            navigation_probe_.finished=true; io.AddMouseButtonEvent(button,false);
            if(free) { io.AddKeyEvent(ImGuiKey_W,false); io.AddKeyEvent(ImGuiKey_E,false); }
        } else if (phase>=2 && free) {
            require(SDL_GetWindowRelativeMouseMode(take(window.native_sdl_window())),"Simulation did not acquire relative mouse");
            SDL_Event e{}; e.type=SDL_EVENT_MOUSE_MOTION; e.motion.windowID=SDL_GetWindowID(take(window.native_sdl_window())); e.motion.xrel=.2f; e.motion.yrel=-.2f;
            require(SDL_PushEvent(&e),"Simulation motion injection failed");
        } else if (phase>=2) {
            const float angle=float(phase-1)*0.06f;
            pointer_={start_pointer_.x+36*std::sin(angle),start_pointer_.y+18*(1-std::cos(angle))};
        }
    }
    if(switch_capture && phase==3) {
        require(ui.simulation_navigating() && SDL_GetWindowRelativeMouseMode(take(window.native_sdl_window())),"Switch test never captured");
        pointer_=control("simulation_edit_view"); io.AddMousePosEvent(pointer_.x,pointer_.y); io.AddMouseButtonEvent(0,true);
    }
    if(switch_capture && phase==4) io.AddMouseButtonEvent(0,false);
    if(switch_capture && phase==5) navigation_pose_=ui.simulation_camera().world().matrix();
    io.AddMousePosEvent(pointer_.x,pointer_.y);
    if (phase==1 && !action.starts_with(":")) io.AddMouseButtonEvent(0,false);
    if(phase==1 && action==":all") { io.AddKeyEvent(ImGuiKey_A,false); io.AddKeyEvent(ImGuiMod_Ctrl,false); }
    if(phase==1 && action==":enter") io.AddKeyEvent(ImGuiKey_Enter,false);
    if (phase<6) return;
    require(ui.errors()==simulation_expected_errors_,"Unexpected control, export or viewport failure");
    const auto state=model.simulation_state(); const auto& shown=ui.displayed_simulation();
    require(model.snapshot()->state.document_id==session_ && model.snapshot()->state.revision==revision_ &&
        model.history().undo_count==history_ && model.snapshot()->scene.same_content(*simulation_scene_),"Simulation changed Edit/history");
    if (state.run) require(!state.run->fault,"Simulation faulted");
    require(viewport.error().empty(),viewport.error().c_str());
    const bool image=state.run && viewport.simulation_run()==state.run->run_id && viewport.texture() && viewport.simulation_view_current();
    bool ready=false;
    if(switch_capture) {
        ready=!viewport.simulation_run() && viewport.current(model.snapshot()->state);
        if(ready) {
            require(!ui.simulation_navigating() && !SDL_GetWindowRelativeMouseMode(take(window.native_sdl_window())),"View switch retained relative mouse");
            require(ui.simulation_camera().world().matrix()==navigation_pose_ && ui.input().camera().world().matrix()==edit_pose_,"View switch continued movement");
            io.AddMouseButtonEvent(1,false); io.AddKeyEvent(ImGuiKey_W,false);
        }
    }
    else if(action=="simulation_camera_apply") {
        ready=image && ui.simulation_camera().fov_y()==(simulation_backend_ ? 75 : 55);
        if(ready) {
            require(ui.input().camera().fov_y()==65,"Simulation lens changed Edit lens");
            require(ui.simulation_camera().revision()!=simulation_camera_revision_,"Simulation lens did not invalidate view");
            require(viewport.capture_simulation_pixels()!=stage_pixels_,"Simulation FOV did not alter the image");
        }
    }
    else if(action.starts_with("simulation_camera_") || action==":all" || action==":fov" || action==":nav_speed" || action==":enter") ready=image;
    else if (action=="sim_solver") ready=ui.controls().contains("sim_gpu");
    else if (action=="sim_gpu") ready=model.simulation_draft().gpu;
    else if (action=="sim_stop") ready=!state.run && !shown.run && !viewport.simulation_run() && viewport.current(model.snapshot()->state);
    else if (action=="simulation_edit_view") ready=!viewport.simulation_run() && viewport.current(model.snapshot()->state);
    else if (state.run && shown.run && shown.run->run_id==state.run->run_id && state.run->task) {
        const auto status=state.run->task->status;
        if (action=="sim_prepare") {
            ready=status==SimulationTaskStatus::paused && shown.run->task->status==status && image;
            if (ready) {
                require(state.run->clock.steps==0 && viewport.simulation_steps()==0,"Prepare advanced before Step");
                require(!state.run->metrics_sample && !state.run->task->performance,"New run inherited old metrics");
                require(state.run->gpu==model.simulation_draft().gpu,"Wrong solver"); simulation_id_=state.run->run_id;
            }
        } else if (action=="sim_step") {
            ready=status==SimulationTaskStatus::paused && shown.run->clock.steps==1 && image && viewport.simulation_steps()==1;
            if (ready) {
                require(state.run->clock.steps==1 && state.run->run_id==simulation_id_,"Step did not preserve run / exact count");
                require(state.run->metrics_sample && state.run->metrics_sample->steps==0,"Step did not preserve stale metrics");
                require(state.run->task->performance && state.run->task->performance->steps==1,"Missing completed batch timing");
            }
        } else if (action=="sim_sample") {
            ready=state.run->metrics_sample && shown.run->metrics_sample && state.run->metrics_sample->steps==state.run->clock.steps &&
                shown.run->metrics_sample->steps==state.run->clock.steps;
            if (ready) require(state.run->metrics_sample->metrics.particle_count==64,"Wrong metrics sample");
        } else if (action=="sim_export" || action=="sim_export_duplicate") {
            const auto& receipt=model.simulation_export();
            ready=receipt && receipt->run_id==state.run->run_id && receipt->steps==1;
            if (ready) {
                require(receipt->simulated_time_ns==10000000,"Wrong export clock");
                require(model.simulation_export_error().empty()==(action=="sim_export"),"Export failure not visible");
                if (action=="sim_export_duplicate") require(export_bytes()==simulation_export_bytes_,"Failed export overwrote valid files");
            }
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
                if (ready) {
                    require(viewport.capture_simulation_pixels()!=pixels_,"Running cloth did not change initial pixels");
                    if (state.run->run_id==simulation_id_) require(state.run->metrics_sample && state.run->metrics_sample->steps==1,"Running overwrote explicit sample");
                }
            }
        } else if (action==":paused") {
            if (image && viewport.simulation_steps()==state.run->clock.steps) {
                const auto hash=viewport.capture_simulation_pixels();
                if (!simulation_stable_) { camera_pixels_=hash; simulation_observed_step_=state.run->clock.steps; }
                require(hash==camera_pixels_ && state.run->clock.steps==simulation_observed_step_,"Paused image or clock changed");
                ready=++simulation_stable_>=8;
            }
        } else if (navigation || action==":zoom" || action=="simulation_frame") {
            ready=(!navigation || navigation_probe_.finished) && image && ui.simulation_camera().revision()!=simulation_camera_revision_;
            if (ready) {
                require(viewport.capture_simulation_pixels()!=stage_pixels_,"Camera input did not alter cloth image");
                if(free) {
                    require(ui.input().camera().world().matrix()==edit_pose_ && ui.input().camera().navigation_mode()==NavigationMode::orbit,"Simulation navigation changed Edit camera");
                    require(!ui.simulation_navigating() && !SDL_GetWindowRelativeMouseMode(take(window.native_sdl_window())),"Simulation mouse release failed");
                    if(action==":walk") require(ui.simulation_camera().world().matrix()(1,3)==navigation_pose_(1,3),"Simulation Walk changed height");
                }
                if (navigation) require(action.ends_with("_running") ? state.run->clock.steps>simulation_observed_step_ :
                    state.run->clock.steps==simulation_observed_step_,"Camera input changed paused steps or starved running simulation");
            }
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

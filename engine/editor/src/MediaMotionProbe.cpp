#include "MediaMotionProbe.hpp"
#include "EditorSupport.hpp"
#include "MediaProbeSupport.hpp"
#include <SDL3/SDL.h>
#include <cmath>
#include <numbers>

namespace dk::editor::detail {
unsigned MediaMotionProbe::render_limit(const std::filesystem::path& root) {
    std::ifstream file(root/".dk-media-motion.json"); Json config; file>>config;
    const auto width=config.value("width",0u);
    if(width>4096 || (width && width<64)) throw std::runtime_error("Invalid probe render limit");
    return width;
}
MediaMotionProbe::MediaMotionProbe(const std::filesystem::path& root):directory_(root/".dk-media-motion") {
    std::ifstream file(root/".dk-media-motion.json"); file>>config_;
    const auto mode=config_.at("algorithm").get<std::string>();
    if((mode!="classic" && mode!="restir") || (config_.at("preset")!="clear" && config_.at("preset")!="sparse") ||
        config_.at("seconds").get<double>()<2 || config_.at("seconds").get<double>()>20)
        throw std::runtime_error("Invalid media motion probe configuration");
    if(!std::filesystem::create_directory(directory_)) throw std::runtime_error("Media probe output must be fresh");
    report_={{"status","running"},{"config",config_},{"frames",Json::array()},{"gui_intervals_ms",Json::array()},
        {"inputs",Json::array()},{"measurement","SDL event injection to completed GUI GPU submission; excludes display scanout"}};
    input_times_.reserve(8192); save();
}
void MediaMotionProbe::save() {
    std::ofstream file(directory_/"report.json"); file<<report_.dump(2); file.close();
    if(!file) throw std::runtime_error("Cannot save media motion report");
}
void MediaMotionProbe::input(const WorkbenchUi& ui,Workspace& model,Viewport& viewport,platform::Window& window) {
    if(done()) return;
    applied_input_=pending_input_;
    if(ticks_++<20) return;
    auto& io=ImGui::GetIO(); io.ConfigInputTrickleEventQueue=false;
    if(stage_==0) {
        viewport.configure_media_probe(config_.at("legacy").get<bool>(),config_.at("images").get<bool>(),config_.value("surface_direct",false));
        check(model.preset_environment(config_.at("preset").get<std::string>())); check(model.apply_environment());
        const auto set=[&](const char* method,Json parameters) {
            parameters["guard"]=take(model.execute_console("editor.viewport.query")).at("guard");
            return take(model.execute_console(Json{{"method",method},{"params",std::move(parameters)}}.dump()));
        };
        report_["viewport"]=set("editor.viewport.set",{{"renderer","pathtrace"},{"medium_mode",config_.value("surface_direct",false) ? "surface_only" : "scene_media"},
            {"lighting_scope",config_.value("surface_direct",false) ? "direct_only" : "full_path"},
            {"motion_preview_long_edge",192},
            {"direct_lighting",config_["algorithm"]=="restir" ? "restir_di" : "classic"},
            {"initial_candidates",config_.value("surface_direct",false) ? 32 : 8},{"temporal_reuse",true},{"spatial_reuse",true},{"target_spp",64},
            {"seed",1},{"bounces",config_.value("surface_direct",false) ? 1 : 4},{"exposure_ev",2.5},{"time_seconds",17}}).at("settings");
        report_["camera"]=set("editor.camera.set",{{"navigation_mode","fly"},{"eye",{8,1.8,0}},
            {"yaw_deg",90},{"pitch_deg",-.9548412539},{"fov_y_deg",65}}).at("settings");
        report_["environment"]=take(model.execute_console("environment.query")).at("environment");
        stage_=1; save(); return;
    }
    if(stage_!=2) return;
    io.AddFocusEvent(true); io.AddMousePosEvent(pointer_.x,pointer_.y);
    const double time=now();
    if(!started_) {
        pointer_=ui.controls().at("viewport"); io.AddMousePosEvent(pointer_.x,pointer_.y); io.AddMouseButtonEvent(1,true);
        motion_start_=time; measure_start_=config_.value("surface_direct",false) ? 1e30 : time+2000; pending_input_=time; started_=true; return;
    }
    if(time<measure_start_+1000*config_.at("seconds").get<double>()) {
        io.AddMouseButtonEvent(1,true);
        if(!SDL_GetWindowRelativeMouseMode(take(window.native_sdl_window()))) throw std::runtime_error("Media probe lost relative look");
        const double next=30*std::sin((time-motion_start_)*std::numbers::pi/1000);
        SDL_Event event{}; event.type=SDL_EVENT_MOUSE_MOTION; event.motion.windowID=SDL_GetWindowID(take(window.native_sdl_window()));
        event.motion.xrel=float(next-last_look_); last_look_=next; pending_input_=time;
        if(!SDL_PushEvent(&event)) throw std::runtime_error("Cannot inject media probe motion");
    } else {
        io.AddMouseButtonEvent(1,false); stop_=time; stage_=3;
        report_["measured_seconds"]=(stop_-measure_start_)/1000;
    }
}
void MediaMotionProbe::presented(const WorkbenchUi& ui,Viewport& viewport) {
    if(!viewport.error().empty()) throw std::runtime_error(viewport.error());
    const double time=now();
    const auto revision=ui.input().camera().revision();
    if(revision!=observed_camera_) {
        observed_camera_=revision; input_times_[revision]=applied_input_;
        if(stage_==2 && applied_input_>=measure_start_) report_["inputs"].push_back({revision,applied_input_-measure_start_});
    }
    if(stage_==1 && (config_.value("surface_direct",false) ? !viewport.path_info().medium : viewport.path_info().medium.has_value()) && viewport.camera_current(ui.input().camera()) && viewport.info().width>192) {
        report_["cold_ready_ms"]=time; report_["full_size"]={viewport.info().width,viewport.info().height};
        report_["initial_source"]=media_probe_source(viewport.path_info(),viewport.published_camera());
        stage_=2; save();
    }
    if(stage_==2 && started_ && config_.value("surface_direct",false) && measure_start_>1e20) {
        if(viewport.published_camera().revision()!=warm_camera_) { warm_camera_=viewport.published_camera().revision(); ++warm_frames_; }
        if(warm_frames_>=20 && time-motion_start_>=2000) { measure_start_=time; report_["warm_camera_updates"]=warm_frames_; }
    }
    if(stage_==2 && started_ && time>=measure_start_) {
        if(previous_present_>=measure_start_) report_["gui_intervals_ms"].push_back(time-previous_present_);
        const auto published=viewport.published_camera().revision();
        if(published!=published_camera_ && input_times_.contains(published) && input_times_.at(published)>=measure_start_ && (config_.value("surface_direct",false) || viewport.info().width<=192)) {
            published_camera_=published;
            const auto& shown=viewport.path_info();
            report_["frames"].push_back({{"at_ms",time-measure_start_},{"input_ms",time-input_times_.at(published)},
                {"request_ms",viewport.path_latency_ms()},{"camera",published},{"current_camera",revision},
                {"width",shown.width},{"height",shown.height},{"spp",shown.samples.count},{"sampler",shown.settings.sampler_version},{"restir_version",shown.restir ? shown.restir->version : 0},
                {"temporal_reused",shown.temporal_statistics.reused},{"temporal_rejected",shown.temporal_statistics.rejected},{"visibility_rays",shown.spatial_statistics.current_visibility_rays}});
            if(config_.at("images").get<bool>() && images_<3 && time-measure_start_>=double(images_)*(config_.value("surface_direct",false) ? 600 : 1000)) {
                auto source=media_probe_source(shown,viewport.published_camera()); source["exposure_ev"]=2.5;
                save_media_linear(directory_/("motion-"+std::to_string(images_++)),viewport.path_linear(),std::move(source));
            }
        }
    }
    previous_present_=time;
    if(stage_==3 && viewport.camera_current(ui.input().camera()) && viewport.info().width>192 && (config_.value("surface_direct",false) ? !viewport.path_info().medium : viewport.path_info().medium.has_value())) {
        if(!restored_spp_) {
            restored_spp_=viewport.path_samples(); report_["stop_to_full_ms"]=time-stop_;
            report_["restored_source"]=media_probe_source(viewport.path_info(),viewport.published_camera());
        } else if(viewport.path_samples()>restored_spp_) {
            report_["stop_to_next_batch_ms"]=time-stop_; report_["next_spp"]=viewport.path_samples();
            report_["status"]="passed"; stage_=4; save();
        }
    }
}
}

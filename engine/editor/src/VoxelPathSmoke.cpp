#include "VoxelPathSmoke.hpp"
#include "EditorSupport.hpp"
#include <dk/io/File.hpp>
#include <fstream>
#include <format>
#include <cctype>
#include <algorithm>
namespace dk::editor::detail {
VoxelPathSmoke::VoxelPathSmoke(const std::filesystem::path& root):directory_(root/".dk-voxel-path") { std::filesystem::create_directories(directory_); }
void VoxelPathSmoke::input(WorkbenchUi& ui,Workspace& model) {
    if(request_.is_null()) {
        if(!std::filesystem::is_regular_file(directory_/"request.json")) return;
        std::ifstream stream(directory_/"request.json");
        if(!stream) return; // Atomic replacement can briefly deny Windows readers.
        Json next; stream>>next;
        const auto name=next.at("name").get<std::string>(); if(name==last_) return;
        if(name.empty() || name.size()>64 || !std::ranges::all_of(name,[](unsigned char c){return std::isalnum(c) || c=='_' || c=='-';}))
            throw std::runtime_error("Invalid voxel path checkpoint name");
        request_=std::move(next); tick_=0;
    }
    auto& io=ImGui::GetIO(); io.ConfigInputTrickleEventQueue=false; io.AddFocusEvent(true);
    auto click=[&](const char* key) {
        const auto it=ui.controls().find(key); if(it==ui.controls().end()) throw std::runtime_error(std::string("Missing voxel path control: ")+key);
        start_=pointer_=it->second; io.AddMousePosEvent(pointer_.x,pointer_.y); io.AddMouseButtonEvent(0,true);
    };
    const auto action=request_.value("action",std::string{});
    if(action=="console_joint") {
        if(tick_==0 || tick_==2) click("console_input");
        if(tick_==1 || tick_==3 || tick_==11) io.AddMouseButtonEvent(0,false);
        if(tick_==4) {io.AddKeyEvent(ImGuiMod_Ctrl,true); io.AddKeyEvent(ImGuiKey_A,true);}
        if(tick_==5) {io.AddKeyEvent(ImGuiKey_A,false); io.AddKeyEvent(ImGuiMod_Ctrl,false);}
        if(tick_==6) {
            if(!ui.console_input().active()) throw std::runtime_error("Voxel path Console did not receive focus");
            const auto state=take(model.execute_console("editor.viewport.query"));
            const auto text="editor.viewport.set guard="+state.at("guard").dump()+" direct_lighting=restir_di temporal_reuse=true spatial_reuse=true";
            io.AddInputCharactersUTF8(text.c_str());
        }
        if(tick_==10) click("console_run");
    } else if(!action.empty()) {
        if(tick_==0) click(action=="gui_path" ? "path_mode" : action=="gui_raster" ? "path_raster" : action=="select" ? "viewport_empty" : action=="gizmo" ? "gizmo_0" : action=="undo" ? "undo" : "camera_frame");
        if(tick_==1 && action!="gizmo") io.AddMouseButtonEvent(0,false);
        if(action=="select" && tick_==4) click("viewport");
        if(action=="select" && tick_==5) io.AddMouseButtonEvent(0,false);
        if(action=="gizmo") {
            if(tick_==3) pointer_={start_.x+24,start_.y};
            if(tick_==5 && !ui.input().active()) throw std::runtime_error("Voxel path Gizmo did not capture entity");
            if(tick_==6) io.AddMouseButtonEvent(0,false);
        }
    }
    if(!action.empty()) io.AddMousePosEvent(pointer_.x,pointer_.y);
    ++tick_;
}
void VoxelPathSmoke::presented(WorkbenchUi& ui,Workspace& model,Viewport& viewport) {
    if(request_.is_null() || tick_<18) return;
    const auto expected=request_.value("expect",std::string{"complete"}); const auto progress=viewport.path_status().progress;
    if(expected=="failed") {if(viewport.error().empty() || viewport.current(model.snapshot()->state)) return;}
    else if(expected=="pending") {if(!progress.pending || progress.completed_spp>=ui.path_settings().target) return;}
    else {
        if(!viewport.error().empty()) throw std::runtime_error("Voxel path checkpoint failed: "+viewport.error());
        if(!viewport.current(model.snapshot()->state) || !viewport.camera_current(ui.input().camera())) return;
        if(ui.path_mode() && viewport.path_samples()!=ui.path_settings().target) return;
    }
    const auto name=request_.at("name").get<std::string>(); auto report=take(model.execute_console("editor.viewport.query"));
    report["checkpoint"]=name; report["voxel_count"]=viewport.info().voxel_count; report["published_path"]=viewport.published_path();
    report["scene_revision"]=model.snapshot()->state.revision; report["dirty"]=model.dirty(); report["undo"]=model.history().undo_count; report["redo"]=model.history().redo_count;
    report["selection"]=model.selection() ? Json(model.selection()->to_string()) : Json(nullptr);
    report["console_history"]=ui.console_input().history_size(); report["console_empty"]=ui.console_input().text().empty(); report["ui_errors"]=ui.errors();
    report["pending"]=progress.pending; report["generation"]=progress.generation; report["reset_reason"]=viewport.path_status().reset_reason;
    report["pixels"]=expected=="pending" ? 0 : viewport.published_path() ? viewport.capture_path_pixels() : viewport.pixel_signature();
    const auto camera=viewport.published_camera(); const auto world=camera.world().matrix();
    const Vec3d eye=world.block<3,1>(0,3),target=eye-world.block<3,1>(0,2),up=world.block<3,1>(0,1);
    report["camera"]={{"eye",{eye.x(),eye.y(),eye.z()}},{"target",{target.x(),target.y(),target.z()}},{"up",{up.x(),up.y(),up.z()}},{"near",camera.near_plane()},{"far",camera.far_plane()}};
    if(camera.projection_mode()==ProjectionMode::orthographic) {report["camera"]["projection"]="orthographic"; report["camera"]["ortho_height"]=camera.settings().lens.ortho_height;}
    else report["camera"]["fov_y"]=camera.fov_y();
    if(expected=="complete") {
        const auto pick=take(viewport.pick({.5,.5})); report["center_pick"]=pick ? Json(pick->to_string()) : Json(nullptr);
        const auto bounds=viewport.bounds(model.snapshot()->scene.entities().front().id,model.snapshot()->scene);
        report["bounds"]={{"min",{bounds.minimum.x(),bounds.minimum.y(),bounds.minimum.z()}},{"max",{bounds.maximum.x(),bounds.maximum.y(),bounds.maximum.z()}}};
    }
    if(ui.path_mode()) {
        const auto& settings=ui.path_settings(); const auto& used=viewport.path_info();
        report["geometry_fingerprint"]=std::format("fnv1a64:{:016x}",used.geometry_fingerprint);
        report["input_fingerprint"]=std::format("fnv1a64:{:016x}",used.fingerprint); report["shown_revision"]=used.revision;
        report["epoch_first"]=used.sampling_first_epoch; report["history_empty"]=!used.temporal_history_epoch;
        report["temporal_reused"]=used.temporal_statistics.reused; report["spatial_reused"]=used.spatial_statistics.reused;
        report["pathtrace"]={{"version",3},{"medium_mode",settings.scene_media ? "scene_media" : "surface_only"},
            {"direct_lighting",settings.restir_di ? "restir_di" : "classic_mis"},{"target_spp",settings.target},{"seed",settings.seed},
            {"max_bounces",settings.bounces},{"rr_start",std::min(3u,settings.bounces)},
            {"environment_rgb",settings.sky_enabled ? Json{settings.sky.x(),settings.sky.y(),settings.sky.z()} : Json{0,0,0}},
            {"sun",settings.sun},{"exposure_ev",settings.exposure}};
        if(settings.restir_di) {
            Json config={{"initial_candidates",settings.restir_candidates}};
            if(settings.restir_temporal) config["temporal"]=Json::object(); if(settings.restir_spatial) config["spatial"]=Json::object();
            report["pathtrace"]["restir"]=std::move(config);
        }
        if(settings.scene_media) report["pathtrace"].update({{"time_seconds",settings.time_seconds},{"cloud_albedo",settings.cloud_albedo},{"cloud_anisotropy",settings.cloud_anisotropy}});
        if(expected=="complete") {
            Json linear=Json::array(); for(const auto& color:viewport.path_linear()) linear.push_back({color.x(),color.y(),color.z()});
            if(linear.size()!=std::size_t(viewport.info().width)*viewport.info().height) throw std::runtime_error("Voxel path linear owner missing");
            const auto text=linear.dump(); check(write_file_bytes_atomic(directory_/(name+".linear.json"),std::as_bytes(std::span{text.data(),text.size()})));
        }
    }
    const auto text=report.dump(2); check(write_file_bytes_atomic(directory_/(name+".json"),std::as_bytes(std::span{text.data(),text.size()})));
    std::printf("Voxel path checkpoint %s passed\n",name.c_str()); std::fflush(stdout); last_=name; request_=nullptr;
}
}

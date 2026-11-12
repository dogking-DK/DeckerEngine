#include "VoxelWorkflowSmoke.hpp"
#include "WorkbenchUi.hpp"
#include "EditorSupport.hpp"
#include <dk/io/File.hpp>
#include <cstdio>
namespace dk::editor::detail {
namespace { constexpr const char* names[]={"base","gui","console","selected","moved","undo","redo","framed","orthographic","failed","recovered","ipc","mesh_front","voxel_front"}; }
VoxelWorkflowSmoke::VoxelWorkflowSmoke(const std::filesystem::path& root):directory_(root/".dk-voxel") { std::filesystem::create_directories(directory_); }
void VoxelWorkflowSmoke::input(WorkbenchUi& ui,Workspace& model) {
    ImGui::GetIO().ConfigInputTrickleEventQueue=false;
    if(waiting_) {
        if(stage_==13 || !std::filesystem::exists(directory_/(std::string(names[stage_])+".continue"))) return;
        ++stage_; tick_=0; waiting_=false; ready_=false;
    }
    auto& io=ImGui::GetIO(); io.AddFocusEvent(true);
    auto click=[&](const char* name) {
        const auto it=ui.controls().find(name); if(it==ui.controls().end()) throw std::runtime_error(std::string("Missing voxel control: ")+name);
        pointer_=start_=it->second; io.AddMousePosEvent(pointer_.x,pointer_.y); io.AddMouseButtonEvent(0,true);
    };
    if(stage_==1) {
        if(tick_==0) click("voxel_settings");
        if(tick_==1 || tick_==5) io.AddMouseButtonEvent(0,false);
        if(tick_==4) click("voxel_directional");
        if(tick_==7 && model.editor_viewport()->settings().voxel.directional) throw std::runtime_error("Voxel draft applied before Apply");
        if(tick_==8) click("voxel_apply");
        if(tick_==9) io.AddMouseButtonEvent(0,false);
        if(tick_==12) click("console_input"); // Outside click closes the nonmodal display popup.
        if(tick_==13) io.AddMouseButtonEvent(0,false);
    } else if(stage_==2) {
        if(tick_==0) click("console_input");
        if(tick_==1) io.AddMouseButtonEvent(0,false);
        if(tick_==2) click("console_input");
        if(tick_==3) io.AddMouseButtonEvent(0,false);
        if(tick_==4) { io.AddKeyEvent(ImGuiMod_Ctrl,true); io.AddKeyEvent(ImGuiKey_A,true); }
        if(tick_==5) { io.AddKeyEvent(ImGuiKey_A,false); io.AddKeyEvent(ImGuiMod_Ctrl,false); }
        if(tick_==6) {
            if(!ui.console_input().active()) throw std::runtime_error("Voxel Console input did not receive focus");
            const auto q=take(model.execute_console("editor.viewport.query"));
            const auto text="editor.viewport.set guard="+q.at("guard").dump()+" voxel_shading=unlit voxel_irradiance=[2,2,2]";
            io.AddInputCharactersUTF8(text.c_str());
        }
        if(tick_==10) click("console_run");
        if(tick_==11) io.AddMouseButtonEvent(0,false);
    } else if(stage_>=3 && stage_<=7) {
        if(tick_==0) click(stage_==3 ? "viewport_empty" : stage_==4 ? "gizmo_0" : stage_==5 ? "undo" : stage_==6 ? "redo" : "camera_frame");
        if(stage_==3 && tick_==4) click("viewport");
        if(stage_==3 && tick_==5) io.AddMouseButtonEvent(0,false);
        if(stage_==4) {
            if(tick_==3) pointer_={start_.x+28,start_.y};
            if(tick_==5 && !ui.input().active()) throw std::runtime_error("Voxel Gizmo did not capture the entity");
            if(tick_==6) io.AddMouseButtonEvent(0,false);
        } else if(tick_==1) io.AddMouseButtonEvent(0,false);
    }
    io.AddMousePosEvent(pointer_.x,pointer_.y); ready_=++tick_>=18;
}
void VoxelWorkflowSmoke::presented(WorkbenchUi& ui,Workspace& model,Viewport& viewport) {
    if(waiting_ || !ready_) return;
    if(stage_!=9 && !viewport.error().empty()) throw std::runtime_error("Voxel viewport failed: "+viewport.error());
    if(tick_==18) {
        std::printf("Voxel stage %s voxels=%zu current=%d camera_current=%d console=%.*s history=%zu ui_errors=%u\n",names[stage_],viewport.info().voxel_count,viewport.current(model.snapshot()->state),viewport.camera_current(ui.input().camera()),int(ui.console_input().text().size()),ui.console_input().text().data(),ui.console_input().history_size(),ui.errors());
    }
    if(stage_==9) { if(viewport.error().empty() || viewport.current(model.snapshot()->state)) return; }
    else if(!viewport.current(model.snapshot()->state) || !viewport.camera_current(ui.input().camera()) || !viewport.error().empty() || viewport.info().voxel_count!=1) return;
    if(stage_==1 && !model.editor_viewport()->settings().voxel.directional) return;
    if(stage_==2 && (model.editor_viewport()->settings().voxel.directional || model.editor_viewport()->settings().voxel.irradiance.x()!=2 || !ui.console_input().text().empty()))
        throw std::runtime_error("Console voxel setting did not execute; text="+std::string{ui.console_input().text()}+" history="+std::to_string(ui.console_input().history_size())+" errors="+std::to_string(ui.errors()));
    if(stage_>=3 && stage_<=7 && !model.selection()) throw std::runtime_error("Voxel GUI selection lost");
    auto report=take(model.execute_console("editor.viewport.query"));
    report["checkpoint"]=names[stage_]; report["pixels"]=viewport.pixel_signature(); report["voxel_count"]=viewport.info().voxel_count;
    report["scene_revision"]=model.snapshot()->state.revision; report["undo"]=model.history().undo_count; report["redo"]=model.history().redo_count;
    report["dirty"]=model.dirty(); report["selection"]=model.selection() ? Json(model.selection()->to_string()) : Json(nullptr);
    report["console_history"]=ui.console_input().history_size(); report["ui_errors"]=ui.errors();
    const auto camera=viewport.published_camera(); const auto world=camera.world().matrix();
    const Vec3d eye=world.block<3,1>(0,3),target=eye-world.block<3,1>(0,2),up=world.block<3,1>(0,1);
    report["camera"]={{"eye",{eye.x(),eye.y(),eye.z()}},{"target",{target.x(),target.y(),target.z()}},{"up",{up.x(),up.y(),up.z()}},
        {"near",camera.near_plane()},{"far",camera.far_plane()}};
    if(camera.projection_mode()==ProjectionMode::orthographic) {
        report["camera"]["projection"]="orthographic"; report["camera"]["ortho_height"]=camera.settings().lens.ortho_height;
    } else report["camera"]["fov_y"]=camera.fov_y();
    if(stage_!=9) {
        const auto selected=take(viewport.pick({.5,.5})); report["center_pick"]=selected ? Json(selected->to_string()) : Json(nullptr);
        const auto& entity=model.snapshot()->scene.entities().front();
        const auto b=viewport.bounds(entity.id,model.snapshot()->scene);
        report["bounds"]={{"min",{b.minimum.x(),b.minimum.y(),b.minimum.z()}},{"max",{b.maximum.x(),b.maximum.y(),b.maximum.z()}}};
    }
    const auto text=report.dump(2); check(write_file_bytes_atomic(directory_/(std::string(names[stage_])+".json"),std::as_bytes(std::span{text.data(),text.size()})));
    const auto bytes=viewport.retained_pixels(); const auto info=viewport.info();
    std::string ppm="P6\n"+std::to_string(info.width)+" "+std::to_string(info.height)+"\n255\n";
    for(std::size_t i=0;i<bytes.size();i+=4) for(unsigned c=0;c<3;++c) ppm+=char(std::to_integer<unsigned char>(bytes[i+c]));
    check(write_file_bytes_atomic(directory_/(std::string(names[stage_])+".ppm"),std::as_bytes(std::span{ppm.data(),ppm.size()})));
    std::printf("Voxel workflow checkpoint %s passed\n",names[stage_]); std::fflush(stdout); waiting_=true;
}
}

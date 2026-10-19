#include "SmokeDriver.hpp"
#include "EditorSupport.hpp"
#include <array>
#include <cstdio>

namespace dk::editor::detail {
bool SmokeDriver::orthographic_input(Workspace& model,WorkbenchUi& ui,Viewport& viewport) {
    constexpr std::array actions={"camera_orthographic","camera_front","camera_back","camera_left","camera_right","camera_top","camera_bottom",
        "camera_reset","camera_frame","viewport_empty","viewport","mode_move",":move","undo","redo",":cancel","mode_rotate",":rotate","mode_scale",":scale",
        ":orbit",":pan",":zoom","camera_frame",":external_environment","unlit_inspection",":disable_unlit",
        "camera_perspective",":restore_environment"};
    if(ortho_stage_>=actions.size()) return true;
    const std::string_view action=actions[ortho_stage_]; const auto phase=ortho_phase_++;
    const auto require=[](bool ok,const char* text) { if(!ok) throw std::runtime_error(std::string("Orthographic smoke: ")+text); };
    const auto control=[&](const char* key) { auto found=ui.controls().find(key); require(found!=ui.controls().end(),key); return found->second; };
    auto& io=ImGui::GetIO(); io.ConfigInputTrickleEventQueue=false;
    const auto& camera=ui.input().camera();
    const bool gizmo=action==":move" || action==":rotate" || action==":scale" || action==":cancel";
    const bool navigation=action==":orbit" || action==":pan";
    const int button=action==":orbit" ? 1 : action==":pan" ? 2 : 0;
    if(phase==0) {
        std::printf("Orthographic begin %s\n",actions[ortho_stage_]);
        io.AddFocusEvent(true); stage_revision_=model.snapshot()->state.revision; history_=model.history().undo_count;
        stage_pixels_=viewport.pixel_signature(); navigation_pose_=camera.world().matrix(); ortho_height_=camera.settings().lens.ortho_height;
        if(action==":external_environment") {
            // Same owner service used by external commands, after a valid orthographic image.
            check(model.preset_environment("clear"));
            auto& center=model.environment_draft()->value["atmosphere"]["planet_center_m"][1]; center=center.get<double>()-1000;
            check(model.apply_environment());
        } else if(action==":restore_environment") { check(model.undo()); }
        else {
            pointer_=control(gizmo ? (action==":rotate" ? "gizmo_2" : "gizmo_0") : navigation || action==":zoom" ? "viewport_empty" :
                action==":disable_unlit" ? "unlit_inspection" : actions[ortho_stage_]);
            start_pointer_=pointer_; io.AddMousePosEvent(pointer_.x,pointer_.y);
            if(action==":zoom") io.AddMouseWheelEvent(0,2); else io.AddMouseButtonEvent(button,true);
        }
    }
    if(phase==2 && (gizmo || navigation)) {
        if(action==":rotate") {
            const auto center=control("gizmo_center"); const float dx=start_pointer_.x-center.x,dy=start_pointer_.y-center.y;
            pointer_={center.x+dx*.8660254f-dy*.5f,center.y+dx*.5f+dy*.8660254f};
        } else pointer_={start_pointer_.x+24,start_pointer_.y+(navigation ? 10.f : 0.f)};
    }
    io.AddMousePosEvent(pointer_.x,pointer_.y);
    if(phase==1 && !gizmo && !navigation) io.AddMouseButtonEvent(0,false);
    if(phase==4 && gizmo) {
        require(ui.input().active(),"Gizmo did not capture");
        require(model.snapshot()->state.revision==stage_revision_ && model.history().undo_count==history_,"preview changed history");
        require(viewport.pixel_signature()!=stage_pixels_,"Gizmo preview did not change pixels");
    }
    if(phase==5) { if(action==":cancel") io.AddKeyEvent(ImGuiKey_Escape,true); io.AddMouseButtonEvent(button,false); }
    if(phase==6) io.AddKeyEvent(ImGuiKey_Escape,false);
    if(phase<11) return false;
    require(ui.errors()==ortho_errors_,"unexpected UI error");
    if(!viewport.camera_current(camera) || !viewport.current(model.snapshot()->state)) return false;
    require(viewport.error().empty(),"preview failed");
    if(action==":external_environment") {
        require(viewport.pixel_signature()!=stage_pixels_,"orthographic environment did not update image");
        require(camera.projection_mode()==ProjectionMode::orthographic,"external environment changed projection");
    }
    if(ortho_stage_<24) require(camera.projection_mode()==ProjectionMode::orthographic,"projection control did not select Orthographic");
    if(ortho_stage_>=1 && ortho_stage_<=6) {
        require(camera.standard_view()==static_cast<StandardView>(ortho_stage_),"six-view button failed");
        require(camera.world().matrix().allFinite(),"six-view basis is nonfinite");
    }
    if(action=="viewport_empty") require(!model.selection(),"empty orthographic pick retained selection");
    if(action=="viewport") require(model.selection()==entity_,"orthographic pick missed framed entity");
    if(gizmo) {
        require(!ui.input().active(),"Gizmo remained active");
        require(model.snapshot()->state.revision==stage_revision_+(action==":cancel" ? 0 : 1) &&
            model.history().undo_count==history_+(action==":cancel" ? 0 : 1),"Gizmo did not commit exactly once/cancel");
        require((viewport.pixel_signature()==stage_pixels_)==(action==":cancel"),"Gizmo image did not follow commit/cancel");
    }
    if(navigation || action==":zoom") {
        require(viewport.pixel_signature()!=stage_pixels_,"orthographic navigation did not change pixels");
        if(action==":zoom") require(camera.world().matrix()==navigation_pose_ && camera.settings().lens.ortho_height!=ortho_height_,"zoom changed pose or ignored height");
        if(action==":orbit") require(camera.standard_view()==StandardView::none,"Orbit retained fixed view");
    }
    if(!gizmo && action!="undo" && action!="redo" && action!=":external_environment" && action!=":restore_environment")
        require(model.snapshot()->state.revision==stage_revision_ && model.history().undo_count==history_,"display controls changed scene/history");
    if(action==":disable_unlit") {
        require(viewport.pixel_signature()!=stage_pixels_ && camera.projection_mode()==ProjectionMode::orthographic,"orthographic Unlit toggle did not render environment");
#ifdef DK_EDITOR_PATHTRACING
        require(!ui.path_mode(),"unlit control changed render mode");
#endif
    }
    if(action=="camera_perspective") require(camera.projection_mode()==ProjectionMode::perspective,"perspective recovery failed");
    std::printf("Orthographic stage %u %s passed\n",ortho_stage_,actions[ortho_stage_]);
    ++ortho_stage_; ortho_phase_=0; return false;
}
}

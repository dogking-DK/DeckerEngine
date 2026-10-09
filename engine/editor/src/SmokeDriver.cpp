#include "SmokeDriver.hpp"
#include "EditorSupport.hpp"
#include <SDL3/SDL.h>
#include <array>
#include <cstdio>
#include <cmath>

namespace dk::editor::detail {
namespace {
void require(bool value,const char* message) { if (!value) throw std::runtime_error(std::string("UI smoke: ")+message); }
constexpr std::array<const char*,26> actions={
    "entity","name",":all",":name","translation",":all",":translation","apply","undo","redo","save","reload","entity",
    "name",":all",":draft","reload","cancel",":close","cancel","revert",":resize",
    "manifest",":all",":missing","open"
};
}
void SmokeDriver::input(Workspace& model,WorkbenchUi& ui,platform::Window& window,const Viewport& viewport) {
    if (simulation_) { simulation_input(model,ui); return; }
    if (interaction_) { interaction_input(model,ui,window,viewport); return; }
    auto& io=ImGui::GetIO();
    // Consume this frame's backend polling and then our deterministic input as one frame.
    io.ConfigInputTrickleEventQueue=false;
    if (tick_++<12 || done_) return;
    const auto elapsed=tick_-13,stage=elapsed/8,phase=elapsed%8;
    if (stage>=actions.size()) { done_=true; return; }
    const std::string action=actions[stage];
    if (phase==0) {
        io.AddFocusEvent(true);
        if (action==":all") { io.AddKeyEvent(ImGuiMod_Ctrl,true); io.AddKeyEvent(ImGuiKey_A,true); }
        else if (action==":name") io.AddInputCharactersUTF8("Workbench smoke entity");
        else if (action==":translation") io.AddInputCharactersUTF8("0.25");
        else if (action==":draft") io.AddInputCharactersUTF8("Discardable draft");
        else if (action==":missing") io.AddInputCharactersUTF8("does-not-exist.json");
        else if (action==":resize") check(window.resize(1360,820));
        else if (action==":close") {
            SDL_Event event{}; event.type=SDL_EVENT_WINDOW_CLOSE_REQUESTED;
            event.window.windowID=SDL_GetWindowID(take(window.native_sdl_window()));
            if (!SDL_PushEvent(&event)) throw std::runtime_error(SDL_GetError());
        } else {
            const auto control=ui.controls().find(action);
            require(control!=ui.controls().end(),("missing control "+action).c_str());
            pointer_=control->second; io.AddMousePosEvent(pointer_.x,pointer_.y); io.AddMouseButtonEvent(0,true);
        }
    }
    io.AddMousePosEvent(pointer_.x,pointer_.y);
    if (phase==1) {
        if (action==":all") { io.AddKeyEvent(ImGuiKey_A,false); io.AddKeyEvent(ImGuiMod_Ctrl,false); }
        else if (!action.starts_with(":")) io.AddMouseButtonEvent(0,false);
    }
    if (phase==5) {
        verify(model,ui,viewport,stage);
        std::printf("UI smoke stage %u %s passed\n",stage,action.c_str());
    }
}
void SmokeDriver::interaction_input(Workspace& model,WorkbenchUi& ui,platform::Window& window,const Viewport& viewport) {
    constexpr std::array interaction_actions={"viewport_empty","viewport",":move","undo","redo",":cancel",
        "mode_rotate",":rotate","mode_scale",":scale",":orbit",":pan",":zoom","camera_reset","camera_frame","camera_reset",
        ":focus_cancel","save","reload","viewport",":resize_cancel",":minimize_cancel"};
    auto& io=ImGui::GetIO(); io.ConfigInputTrickleEventQueue=false;
    if (tick_++<12 || done_) return;
    const auto elapsed=tick_-13,stage=elapsed/12,phase=elapsed%12;
    if (stage>=interaction_actions.size()) { done_=true; return; }
    const std::string action=interaction_actions[stage];
    const bool cancelled=action==":cancel" || action==":focus_cancel" || action==":resize_cancel" || action==":minimize_cancel";
    const bool gizmo=action==":move" || action==":rotate" || action==":scale" || cancelled;
    const bool navigation=action==":orbit" || action==":pan";
    const int button=action==":orbit" ? 1 : action==":pan" ? 2 : 0;
    const auto control=[&](const std::string& name) {
        const auto found=ui.controls().find(name); require(found!=ui.controls().end(),("missing control "+name).c_str()); return found->second;
    };
    if (phase==0) {
        io.AddFocusEvent(true);
        history_=model.history().undo_count; stage_revision_=model.snapshot()->state.revision; stage_pixels_=viewport.pixel_signature();
        if (gizmo) pointer_=control(action==":rotate" ? "gizmo_2":"gizmo_0");
        else if (navigation || action==":zoom") pointer_=control("viewport_empty");
        else pointer_=control(action);
        start_pointer_=pointer_;
        io.AddMousePosEvent(pointer_.x,pointer_.y);
        if (action==":zoom") io.AddMouseWheelEvent(0,2);
        else io.AddMouseButtonEvent(button,true);
    }
    if (phase==2 && (gizmo || navigation)) {
        if (action==":rotate") {
            const auto center=control("gizmo_center");
            const float dx=start_pointer_.x-center.x,dy=start_pointer_.y-center.y;
            pointer_={center.x+dx*0.8660254f-dy*0.5f,center.y+dx*0.5f+dy*0.8660254f};
        } else pointer_={start_pointer_.x+28,start_pointer_.y+(navigation ? 12.0f : 0.0f)};
    }
    io.AddMousePosEvent(pointer_.x,pointer_.y);
    if (phase==1 && !gizmo && !navigation && action!=":zoom") io.AddMouseButtonEvent(0,false);
    if (phase==4 && gizmo) {
        require(ui.input().active(),"gizmo press did not capture a transform");
        require(model.snapshot()->state.revision==stage_revision_ && model.history().undo_count==history_,"preview modified committed scene/history");
        require(viewport.pixel_signature()!=stage_pixels_,"drag preview did not alter image");
    }
    if (phase==5) {
        if (action==":cancel") io.AddKeyEvent(ImGuiKey_Escape,true);
        else if (action==":focus_cancel") {
            SDL_Event event{}; event.type=SDL_EVENT_WINDOW_FOCUS_LOST;
            event.window.windowID=SDL_GetWindowID(take(window.native_sdl_window()));
            require(SDL_PushEvent(&event),"focus event injection failed");
        }
        else if (action==":resize_cancel") check(window.resize(1360,820));
        else if (action==":minimize_cancel") { check(window.minimize()); SDL_Delay(20); check(window.restore()); }
        else if (gizmo || navigation) io.AddMouseButtonEvent(button,false);
    }
    if (phase==6) {
        if (action==":cancel") io.AddKeyEvent(ImGuiKey_Escape,false);
        if (cancelled) { io.AddMouseButtonEvent(0,false); io.AddFocusEvent(true); }
    }
    if (phase==9) {
        require(ui.errors()==0,"unexpected interaction error"); require(!ui.input().active(),"gesture still active");
        const auto* s=model.snapshot();
        if (stage==0) require(!model.selection(),"empty image click did not clear selection");
        if (stage==1) {
            require(model.draft() && model.draft()->entity.name=="mask","CPU pick did not return nearest mesh entity");
            entity_=model.selection(); session_=s->state.document_id; pixels_=viewport.pixel_signature();
        }
        if (gizmo) {
            if (cancelled) {
                require(s->state.revision==stage_revision_ && model.history().undo_count==history_ &&
                    (action==":resize_cancel" || viewport.pixel_signature()==stage_pixels_),"cancel failed to restore preview without a commit");
            } else {
                require(s->state.revision==stage_revision_+1 && model.history().undo_count==history_+1 && viewport.pixel_signature()!=stage_pixels_,"drag did not commit exactly once");
            }
        }
        if (stage==3) require(viewport.pixel_signature()==pixels_ && model.history().redo_count==1,"Undo did not restore picked image");
        if (stage==4) require(viewport.pixel_signature()!=pixels_,"Redo did not restore moved image");
        if (stage==9) { camera_pixels_=viewport.pixel_signature(); transformed_=model.draft()->entity.local; }
        if (navigation || action==":zoom" || action=="camera_frame") {
            require(s->state.revision==stage_revision_ && model.history().undo_count==history_,"camera modified scene/history");
            require(viewport.pixel_signature()!=stage_pixels_,"camera input did not alter image");
        }
        if (action=="camera_reset") require(viewport.pixel_signature()==camera_pixels_,"camera reset did not restore image");
        if (action=="save") require(!model.dirty(),"interaction save left dirty state");
        if (action=="reload") {
            require(s->state.document_id!=session_ && !model.selection() && model.history().undo_count==0,"reload did not replace interaction session");
            const auto found=std::ranges::find(s->scene.entities(),*entity_,&EntityData::id);
            require(found!=s->scene.entities().end() && found->local.translation.isApprox(transformed_.translation) &&
                found->local.scale.isApprox(transformed_.scale) && found->local.rotation.angularDistance(transformed_.rotation)<1e-10,"saved Gizmo transform not reloaded");
        }
        if (stage==19) require(model.selection()==entity_,"reloaded mesh pick did not recover the selected entity");
        std::printf("Interaction smoke stage %u %s passed\n",stage,action.c_str());
    }
}
void SmokeDriver::verify(Workspace& model,WorkbenchUi& ui,const Viewport& viewport,unsigned stage) {
    require(!ui.closing(),"window unexpectedly closed");
    const auto* s=model.snapshot(); require(s!=nullptr,"lost scene");
    if (stage<25) require(ui.errors()==0,"unexpected command/viewport error");
    switch (stage) {
    case 0:
        require(model.draft()!=nullptr,"Hierarchy click did not select");
        entity_=model.selection(); session_=s->state.document_id;
        name_=model.draft()->entity.name; revision_=s->state.revision;
        translation_=model.draft()->entity.local.translation.x(); pixels_=viewport.pixel_signature(); break;
    case 3: require(model.pending() && model.draft()->entity.name=="Workbench smoke entity","name typing did not reach Inspector"); break;
    case 7: require(model.draft()->entity.local.translation.x()==0.25 && viewport.pixel_signature()!=pixels_,"transform edit did not change viewport pixels"); require(!model.pending() && s->state.dirty && s->state.revision>revision_ && model.history().undo_count==1,"Apply did not commit one transaction"); break;
    case 8: require(model.draft()->entity.local.translation.x()==translation_ && viewport.pixel_signature()==pixels_,"Undo did not restore transform/image"); require(model.draft()->entity.name==name_ && model.history().redo_count==1,"Undo did not restore name"); break;
    case 9: require(model.draft()->entity.local.translation.x()==0.25 && viewport.pixel_signature()!=pixels_,"Redo did not restore transform/image"); require(model.draft()->entity.name=="Workbench smoke entity","Redo did not restore edit"); break;
    case 10: require(!model.dirty(),"Save left dirty state"); break;
    case 11: {
        require(s->state.document_id!=session_ && !model.selection() && model.history().undo_count==0,"Reload did not replace session");
        bool found=false;
        for (const auto& e:s->scene.entities()) if (e.id==entity_) found=e.name=="Workbench smoke entity" && e.local.translation.x()==0.25;
        require(found,"saved name was not loaded from disk"); session_=s->state.document_id; break;
    }
    case 15: require(model.pending(),"second draft missing"); break;
    case 17:
    case 19: require(model.pending() && s->state.document_id==session_,"Cancel lost draft/session"); break;
    case 20: require(!model.pending() && model.draft()->entity.name=="Workbench smoke entity","Revert did not restore committed entity"); break;
    case 25: require(ui.errors()==1 && s->state.document_id==session_ && model.selection()==entity_,"failed Open replaced active scene"); break;
    default: break;
    }
}
}

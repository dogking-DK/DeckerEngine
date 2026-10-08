#include "SmokeDriver.hpp"
#include "EditorSupport.hpp"
#include <SDL3/SDL.h>
#include <array>
#include <cstdio>

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

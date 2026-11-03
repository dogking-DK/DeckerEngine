#include "ConsoleCompletionSmoke.hpp"
#include "WorkbenchUi.hpp"
#include <SDL3/SDL.h>
#include <cstdio>

namespace dk::editor::detail {
namespace {
void require(bool value,const char* message) { if(!value) throw std::runtime_error(std::string("Console completion smoke: ")+message); }
constexpr const char* preset="environment.preset name=";
constexpr const char* legacy=R"({"method":"scene.query","params":{"limit":1}})";
std::string oversized() { return std::string(console_input_limit-10,' ')+"scene.qu"; }
}
ConsoleCompletionSmoke::~ConsoleCompletionSmoke() { restore(); }
void ConsoleCompletionSmoke::restore() {
    if(!installed_) return;
    auto& p=ImGui::GetPlatformIO(); p.Platform_GetClipboardTextFn=clipboard_get_; p.Platform_ClipboardUserData=clipboard_user_; installed_=false;
}
bool ConsoleCompletionSmoke::input(Workspace& model,WorkbenchUi& ui,unsigned window_id) {
    const auto stage=tick_/8,phase=tick_++%8;
    if(stage==66) { restore(); return true; }
    auto& io=ImGui::GetIO();
    const auto click=[&](ImVec2 at) { pointer_=at; io.AddMousePosEvent(at.x,at.y); io.AddMouseButtonEvent(0,true); };
    const auto control=[&](const char* name) { const auto it=ui.controls().find(name); require(it!=ui.controls().end(),"missing control"); click(it->second); };
    const auto compose=[&](const char* text,bool commit) {
        SDL_Event e{};
        if(commit) { e.type=SDL_EVENT_TEXT_INPUT; e.text.windowID=window_id; e.text.text=text; }
        else { e.type=SDL_EVENT_TEXT_EDITING; e.edit.windowID=window_id; e.edit.text=text; e.edit.length=1; }
        require(SDL_PushEvent(&e),"composition injection failed");
    };
    if(phase==0) {
        io.AddFocusEvent(true);
        if(stage==0) {
            errors_=ui.errors(); history_=ui.console_input().history_size(); revision_=model.snapshot()->state.revision;
            entities_=model.snapshot()->scene.entities().size();
            auto& p=ImGui::GetPlatformIO(); clipboard_get_=p.Platform_GetClipboardTextFn; clipboard_user_=p.Platform_ClipboardUserData;
            p.Platform_GetClipboardTextFn=[](ImGuiContext*) { return static_cast<ConsoleCompletionSmoke*>(ImGui::GetPlatformIO().Platform_ClipboardUserData)->clipboard_.c_str(); };
            p.Platform_ClipboardUserData=this; installed_=true; control("console_input");
        }
        switch(stage) {
        case 1: case 21: case 41: case 45: case 50: case 54: case 58: case 63:
            io.AddKeyEvent(ImGuiMod_Ctrl,true); io.AddKeyEvent(ImGuiKey_A,true); break;
        case 2: case 22: io.AddInputCharactersUTF8("scene.qu"); break;
        case 3: case 8: case 9: case 13: case 17: case 23: case 24: case 28: case 31: case 35: case 40: case 43: case 47: case 52: case 53: case 56: case 60: case 65:
            io.AddKeyEvent(ImGuiKey_Tab,true); if(stage==53) ++errors_; break;
        case 4: case 36: io.AddKeyEvent(ImGuiKey_Enter,true); break;
        case 6: case 19: case 26: case 29: case 37: io.AddKeyEvent(ImGuiKey_Enter,true); ++history_; break;
        case 7: io.AddInputCharactersUTF8("scene.query li"); break;
        case 10: io.AddInputCharactersUTF8("2"); break;
        case 11: control("console_run"); ++history_; break;
        case 12: case 30: case 55: case 64: io.AddInputCharactersUTF8(preset); break;
        case 14: case 25: case 62: io.AddKeyEvent(ImGuiKey_DownArrow,true); break;
        case 15: case 20: case 59: io.AddKeyEvent(ImGuiKey_UpArrow,true); break;
        case 16: case 57: io.AddKeyEvent(ImGuiKey_Escape,true); break;
        case 18: {
            const auto& points=ui.console_input().candidate_positions(); require(!points.empty(),"no candidate hit target"); click(points.front()); break;
        }
        case 27: io.AddInputCharactersUTF8(legacy); break;
        case 32: compose("候选",false); break;
        case 33:
            for(auto key : {SDLK_UP,SDLK_DOWN,SDLK_TAB,SDLK_RETURN,SDLK_ESCAPE}) {
                SDL_Event e{}; e.type=SDL_EVENT_KEY_DOWN; e.key.windowID=window_id; e.key.key=key; e.key.down=true;
                e.key.scancode=SDL_GetScancodeFromKey(key,nullptr); require(SDL_PushEvent(&e),"composition key failed");
                e.type=SDL_EVENT_KEY_UP; e.key.down=false; require(SDL_PushEvent(&e),"composition release failed");
            }
            break;
        case 34: compose("cl",true); break;
        case 38: io.AddInputCharactersUTF8("unknown.command"); break;
        case 42: io.AddInputCharactersUTF8("environment.preset name=cl"); break;
        case 44: control("console_run"); ++errors_; ++history_; break;
        case 46: io.AddInputCharactersUTF8(preset); break;
        case 48: control("manifest"); break;
        case 49: control("console_input"); break;
        case 51:
            clipboard_=oversized(); io.AddKeyEvent(ImGuiMod_Ctrl,true); io.AddKeyEvent(ImGuiKey_V,true); break;
        case 61: io.AddInputCharactersUTF8("e"); io.AddKeyEvent(ImGuiKey_Tab,true); ++errors_; break;
        default: break;
        }
    }
    io.AddMousePosEvent(pointer_.x,pointer_.y);
    if(phase==1) {
        io.AddMouseButtonEvent(0,false);
        for(auto key : {ImGuiKey_A,ImGuiKey_V,ImGuiKey_Tab,ImGuiKey_Enter,ImGuiKey_Escape,ImGuiKey_UpArrow,ImGuiKey_DownArrow}) io.AddKeyEvent(key,false);
        io.AddKeyEvent(ImGuiMod_Ctrl,false);
    }
    if(phase!=5) return false;
    const auto& input=ui.console_input(); const auto text=input.text();
    require(ImGui::GetPlatformIO().Platform_ClipboardUserData==this,"clipboard scope not restored");
    require(ui.errors()==errors_,"unexpected error count"); require(input.history_size()==history_,"acceptance or navigation submitted history");
    require(model.snapshot()->state.revision==revision_ && model.snapshot()->scene.entities().size()==entities_,"assistance changed scene");
    switch(stage) {
    case 3: require(input.popup_open() && text=="scene.qu","Tab did not open or altered text"); break;
    case 4: case 5: case 24: case 25: require(!input.popup_open() && text=="scene.query","accept/history detach changed text"); break;
    case 6: case 11: case 19: case 26: case 29: case 37: require(text.empty(),"explicit submission did not succeed"); break;
    case 8: require(input.popup_open() && input.assistance().candidates.front().insert=="limit=","parameter candidates missing"); break;
    case 9: require(!input.popup_open() && text=="scene.query limit=","Tab did not accept parameter"); break;
    case 10: require(input.assistance().hint.find("default: 128")!=std::string::npos,"shared parameter help missing"); break;
    case 13: require(input.popup_open() && input.assistance().candidates.size()>1,"enum candidates missing"); break;
    case 14: require(input.popup_open() && input.selected_candidate()==1 && text==preset,"Down changed history instead of selection"); break;
    case 15: require(input.popup_open() && input.selected_candidate()==0,"Up did not select candidate"); break;
    case 16: require(!input.popup_open() && text==preset,"Escape reverted or cleared input"); break;
    case 18: case 20: case 36: require(text=="environment.preset name=\"clear\"" && !input.popup_open(),"quoted enum acceptance or recall failed"); break;
    case 28: require(!input.popup_open() && text==legacy,"legacy JSON Tab changed input"); break;
    case 32: case 33: require(!input.popup_open() && text==preset,"IME keys changed input or accepted candidate"); break;
    case 34: require(text=="environment.preset name=cl","IME commit failed"); break;
    case 40: require(!input.popup_open() && text=="unknown.command","no-candidate Tab changed input"); break;
    case 44: require(!input.popup_open() && text=="environment.preset name=cl","Run accepted candidate rather than submitting original"); break;
    case 48: require(!input.popup_open() && text==preset,"focus loss retained popup or changed text"); break;
    case 51: case 53: require(text==oversized(),"oversized completion changed input"); if(stage==53) require(!input.popup_open(),"rejected completion retained popup"); break;
    case 56: case 65: require(input.popup_open(),"final assistance preview missing"); break;
    case 59: require(text=="environment.preset name=cl","failed history entry missing"); break;
    case 61: case 62: require(text=="environment.preset name=cle" && !input.popup_open(),"same-frame edit/completion did not reject stale candidate or detach history"); break;
    default: break;
    }
    std::printf("Console completion smoke stage %u passed\n",stage); return false;
}
}

#include "ConsoleSmoke.hpp"
#include "WorkbenchUi.hpp"
#include <SDL3/SDL.h>
#include <cstdio>

namespace dk::editor::detail {
namespace {
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(std::string("Console smoke: ") + message);
}
constexpr const char* query = "scene.query";
constexpr const char* invalid = "scene.query limit=oops";
constexpr const char* draft = "scene.query limit=3";
constexpr const char* legacy = R"({"method":"scene.query","params":{"limit":1}})";
std::string boundary() { return std::string(query) + std::string(console_input_limit-std::string_view(query).size(),' '); }
}
ConsoleSmoke::~ConsoleSmoke() { restore_clipboard(); }
void ConsoleSmoke::restore_clipboard() {
    if (!clipboard_installed_) return;
    auto& platform = ImGui::GetPlatformIO();
    platform.Platform_GetClipboardTextFn = clipboard_get_;
    platform.Platform_ClipboardUserData = clipboard_user_;
    clipboard_installed_ = false;
}
bool ConsoleSmoke::input(Workspace& model, WorkbenchUi& ui, unsigned window_id) {
    const auto stage = tick_/8, phase = tick_++%8;
    if (stage >= 48) { restore_clipboard(); return completion_.input(model,ui,window_id); }
    auto& io = ImGui::GetIO();
    const auto click = [&](const char* name, bool down) {
        const auto found = ui.controls().find(name);
        require(found != ui.controls().end(),"missing console control");
        pointer_ = found->second;
        io.AddMousePosEvent(pointer_.x,pointer_.y); io.AddMouseButtonEvent(0,down);
    };
    const auto paste = [&](std::string value) {
        clipboard_ = std::move(value); io.AddKeyEvent(ImGuiMod_Ctrl,true); io.AddKeyEvent(ImGuiKey_V,true);
    };
    const auto guard = [&] {
        const auto& state = model.snapshot()->state;
        return Json{{"document_id",state.document_id.to_string()},{"revision",state.revision}}.dump();
    };
    const auto composing = [&](const char* text, bool commit) {
        SDL_Event event{};
        if (commit) { event.type=SDL_EVENT_TEXT_INPUT; event.text.windowID=window_id; event.text.text=text; }
        else { event.type=SDL_EVENT_TEXT_EDITING; event.edit.windowID=window_id; event.edit.text=text; event.edit.length=1; }
        require(SDL_PushEvent(&event),"cannot queue composition event");
    };
    if (phase == 0) {
        io.AddFocusEvent(true);
        if (stage == 0) {
            errors_ = ui.errors(); history_ = ui.console_input().history_size();
            revision_ = model.snapshot()->state.revision; entities_ = model.snapshot()->scene.entities().size();
            auto& platform = ImGui::GetPlatformIO();
            clipboard_get_ = platform.Platform_GetClipboardTextFn; clipboard_user_ = platform.Platform_ClipboardUserData;
            platform.Platform_GetClipboardTextFn = [](ImGuiContext*) {
                return static_cast<ConsoleSmoke*>(ImGui::GetPlatformIO().Platform_ClipboardUserData)->clipboard_.c_str();
            };
            platform.Platform_ClipboardUserData = this; clipboard_installed_ = true;
            click("console_input",true);
        }
        switch (stage) {
        case 1: io.AddInputCharactersUTF8(query); break;
        case 2: case 25: case 31: case 45: io.AddKeyEvent(ImGuiKey_Enter,true); if(stage!=31) ++history_; break;
        case 3: case 9: case 10: case 13: case 34: case 43: io.AddKeyEvent(ImGuiKey_UpArrow,true); break;
        case 4: case 11: case 12: case 15: case 44: io.AddKeyEvent(ImGuiKey_DownArrow,true); break;
        case 5: io.AddInputCharactersUTF8(invalid); break;
        case 6: case 16: case 35: io.AddKeyEvent(ImGuiKey_Enter,true); ++errors_; if(stage!=35) ++history_; break;
        case 7: case 17: case 29: case 36: io.AddKeyEvent(ImGuiMod_Ctrl,true); io.AddKeyEvent(ImGuiKey_A,true); break;
        case 8: case 39: io.AddInputCharactersUTF8(draft); break;
        case 14: io.AddInputCharactersUTF8("='中文 空格'"); break;
        case 18: io.AddInputCharactersUTF8(legacy); break;
        case 19: case 38: click("console_run",true); ++history_; break;
        case 20: paste("scene.query\nscene.query"); ++errors_; break;
        case 21: paste(std::string(8193,'x')); ++errors_; break;
        case 22: { std::string s; for (int i=0;i<2731;++i) s+="中"; paste(std::move(s)); ++errors_; break; }
        case 23: case 30: paste(boundary()); break;
        case 24: io.AddInputCharactersUTF8("x"); ++errors_; break;
        case 26: paste(query); break;
        case 27: io.AddKeyEvent(ImGuiKey_End,true); break;
        case 28: paste(boundary()); ++errors_; break;
        case 32: create_="entity.create guard="+guard(); io.AddInputCharactersUTF8(create_.c_str()); break;
        case 33: click("console_run",true); break;
        case 37: { const auto undo="history.undo guard="+guard(); io.AddInputCharactersUTF8(undo.c_str()); break; }
        case 40: composing("候选",false); break;
        case 41: {
            for (auto key : {SDLK_UP,SDLK_RETURN}) {
                SDL_Event event{}; event.type=SDL_EVENT_KEY_DOWN; event.key.windowID=window_id; event.key.key=key;
                event.key.scancode=key==SDLK_UP ? SDL_SCANCODE_UP : SDL_SCANCODE_RETURN; event.key.down=true;
                require(SDL_PushEvent(&event),"cannot queue composition key");
                event.type=SDL_EVENT_KEY_UP; event.key.down=false;
                require(SDL_PushEvent(&event),"cannot queue composition key release");
            }
            break;
        }
        case 42: composing("4",true); break;
        case 46: paste("  \t"); break;
        case 47: io.AddKeyEvent(ImGuiKey_Enter,true); break;
        default: break;
        }
    }
    io.AddMousePosEvent(pointer_.x,pointer_.y);
    if (phase == 1) {
        if (stage==0 || stage==19 || stage==38) io.AddMouseButtonEvent(0,false);
        if (stage==33) {
            // Release Run while Enter is pressed, as can happen with overlapping user input.
            io.AddKeyEvent(ImGuiKey_Enter,true); click("console_run",false); ++history_;
        }
        for (auto key : {ImGuiKey_A,ImGuiKey_V,ImGuiKey_UpArrow,ImGuiKey_DownArrow,ImGuiKey_End}) io.AddKeyEvent(key,false);
        io.AddKeyEvent(ImGuiMod_Ctrl,false);
        if(stage!=33) io.AddKeyEvent(ImGuiKey_Enter,false);
    }
    if (phase==2 && stage==33) io.AddKeyEvent(ImGuiKey_Enter,false);
    if (phase != 5) return false;
    const auto text = ui.console_input().text();
    require(ImGui::GetPlatformIO().Platform_ClipboardUserData==this,"input did not restore clipboard callback user data");
    require(ui.errors()==errors_,"unexpected error count");
    require(ui.console_input().history_size()==history_,"history duplicated or changed on refill/rejection");
    require(model.snapshot()->state.revision==revision_+(stage>=38 ? 2 : stage>=33 ? 1 : 0),"submission count or revision changed unexpectedly");
    require(model.snapshot()->scene.entities().size()==entities_+(stage>=33 && stage<38 ? 1 : 0),"scene changed on navigation/rejection");
    switch (stage) {
    case 0: require(ui.console_input().active(),"Console input did not focus"); break;
    case 1: case 3: case 10: case 26: case 27: case 28: require(text==query,"query recall/paste not exact"); break;
    case 2: case 4: case 19: case 20: case 21: case 22: case 25: case 31: case 33: case 38: case 45:
        require(text.empty(),"successful submission did not clear, or rejected paste altered input"); break;
    case 5: case 6: case 9: case 11: case 13: require(text==invalid,"failed input/history not preserved"); break;
    case 8: case 12: case 39: case 40: case 41: require(text==draft,"draft lost or composition key submitted/recalled history"); break;
    case 14: case 15: case 16: require(text==std::string(invalid)+"='中文 空格'","editing Unicode history did not detach browsing"); break;
    case 18: require(text==legacy,"legacy JSON typing changed"); break;
    case 23: case 24: case 30: require(text==boundary(),"boundary paste truncated or selected replacement rejected"); break;
    case 32: case 34: case 35: require(text==create_,"guard rewritten or failed command lost"); break;
    case 42: case 44: require(text==std::string(draft)+"4","IME commit/draft restore failed"); break;
    case 43: require(text.starts_with("history.undo guard="),"Up did not resume after composition"); break;
    case 46: case 47: require(text=="  \t","blank input should remain without dispatch"); break;
    default: break;
    }
    std::printf("Console smoke stage %u passed\n",stage);
    return false;
}
} // namespace dk::editor::detail

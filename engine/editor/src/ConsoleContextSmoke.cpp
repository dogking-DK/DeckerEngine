#include "ConsoleContextSmoke.hpp"
#include "WorkbenchUi.hpp"
#include <algorithm>
#include <cstdio>

namespace dk::editor::detail {
namespace {
void require(bool value,const char* message) { if(!value) throw std::runtime_error(std::string("Console context smoke: ")+message); }
}
bool ConsoleContextSmoke::input(Workspace& model,WorkbenchUi& ui) {
    const auto stage=tick_/8,phase=tick_++%8;
    if(stage==46) return true;
    auto& io=ImGui::GetIO();
    if(phase==0) {
        action_time_=ImGui::GetTime(); io.AddFocusEvent(true);
        if(stage==0) { errors_=ui.errors(); history_=ui.console_input().history_size(); revision_=model.snapshot()->state.revision; }
        switch(stage) {
        case 0: case 12: case 39: case 42: io.AddKeyEvent(ImGuiKey_Escape,true); break;
        case 1: case 13: case 26: case 29: case 36: case 40: case 43: io.AddKeyEvent(ImGuiMod_Ctrl,true); io.AddKeyEvent(ImGuiKey_A,true); break;
        case 2: case 30: io.AddInputCharactersUTF8("entity.get id="); break;
        case 3: case 7: case 8: case 10: case 15: case 16: case 18: case 31: case 33: case 38: case 45: io.AddKeyEvent(ImGuiKey_Tab,true); break;
        case 4: case 11: case 19: io.AddKeyEvent(ImGuiKey_Enter,true); break;
        case 5: case 21: io.AddKeyEvent(ImGuiKey_Enter,true); ++history_; break;
        case 6: io.AddInputCharactersUTF8("entity.set_assets assets=[{\"kind\":\"me"); break;
        case 9: io.AddInputCharactersUTF8(",\"id\":"); break;
        case 14: io.AddInputCharactersUTF8("{\"method\":\"environment.preset\",\"params\":{\"na"); break;
        case 17: io.AddInputCharactersUTF8("\"cl"); break;
        case 20: io.AddInputCharactersUTF8("}}"); break;
        case 22: io.AddInputCharactersUTF8("scene.query limit="); break;
        case 24: io.AddInputCharactersUTF8("1.0"); break;
        case 27: io.AddInputCharactersUTF8("scene.query limit=1 limit=2"); break;
        case 32: {
            const auto& state=model.snapshot()->state;
            first_entity_=model.snapshot()->scene.entities().front().id.to_string();
            const auto text="entity.set_name id="+first_entity_+" name=\"候选 重命名\" guard="+
                Json{{"document_id",state.document_id.to_string()},{"revision",state.revision}}.dump();
            require(bool(model.execute_console(text)),"fixture rename failed"); ++revision_; break;
        }
        case 35: {
            const auto& state=model.snapshot()->state;
            require(bool(model.execute_console("history.undo guard="+Json{{"document_id",state.document_id.to_string()},{"revision",state.revision}}.dump())),"fixture undo failed");
            ++revision_; break;
        }
        case 37: case 44: io.AddInputCharactersUTF8("entity.set_assets assets=[{\"kind\":\"mesh\",\"id\":"); break;
        case 41: io.AddInputCharactersUTF8("environment.preset name="); break;
        default: break;
        }
    }
    if(phase==1) {
        for(auto key : {ImGuiKey_Escape,ImGuiKey_A,ImGuiKey_Tab,ImGuiKey_Enter}) io.AddKeyEvent(key,false);
        io.AddKeyEvent(ImGuiMod_Ctrl,false);
    }
    // Open/select before debounce settles; later publication must retain the chosen identity.
    if(stage==41) {
        if(phase==1) io.AddKeyEvent(ImGuiKey_Tab,true);
        if(phase==2) { io.AddKeyEvent(ImGuiKey_Tab,false); io.AddKeyEvent(ImGuiKey_DownArrow,true); }
        if(phase==3) io.AddKeyEvent(ImGuiKey_DownArrow,false);
    }
    if(phase!=5) return false;
    // Real UI clock, no blocking sleep or assumptions about GPU frame rate.
    if(ImGui::GetTime()-action_time_<.14) { --tick_; return false; }
    const auto& input=ui.console_input(); const auto& a=input.assistance(); const auto text=input.text();
    require(ui.errors()==errors_,"diagnostics wrote errors to the command log");
    require(input.history_size()==history_,"assistance submitted history");
    require(model.snapshot()->state.revision==revision_,"assistance changed the scene");
    const auto diagnostic=[&](ConsoleDiagnosticKind kind,std::string_view message) {
        return std::ranges::any_of(a.diagnostics,[&](const auto& d){return d.kind==kind && d.message.find(message)!=std::string::npos;});
    };
    switch(stage) {
    case 3: case 31:
        require(input.popup_open() && a.provider=="scene.entity" && !a.loading && !a.candidates.empty(),"entity candidates not loaded");
        require(a.candidates.front().label.find('[')!=std::string::npos,"candidate identity label missing"); break;
    case 4: require(text.starts_with("entity.get id=\"") && !input.popup_open(),"entity completion not quoted or submitted"); break;
    case 5: case 21: require(text.empty(),"explicit query failed"); break;
    case 8: require(text=="entity.set_assets assets=[{\"kind\":\"mesh\"","nested enum not completed"); break;
    case 10: case 38: case 45:
        require(input.popup_open() && a.provider=="assets.reference" && !a.loading && a.candidates.size()==2,"asset candidates not loaded");
        require(a.kind_filter=="mesh","asset kind filter missing"); break;
    case 11: require(text.find("\"id\":\"10000000-")!=std::string_view::npos,"asset UUID not inserted"); break;
    case 16: require(text=="{\"method\":\"environment.preset\",\"params\":{\"name\":","envelope property completion failed"); break;
    case 19: require(text.ends_with("\"name\":\"clear\""),"envelope enum completion failed"); break;
    case 23: require(diagnostic(ConsoleDiagnosticKind::incomplete,"value"),"missing value not incomplete"); break;
    case 25: require(diagnostic(ConsoleDiagnosticKind::error,"type"),"integer token error not shown"); break;
    case 28: require(diagnostic(ConsoleDiagnosticKind::error,"Duplicate"),"duplicate parameter not shown"); break;
    case 32: case 35: require(!input.popup_open(),"scene revision did not invalidate popup"); break;
    case 34: require(std::ranges::any_of(a.candidates,[](const auto& c){return c.label.find("候选 重命名")!=std::string::npos;}),"renamed entity not refreshed"); break;
    case 41: require(input.popup_open() && input.selected_candidate()==1 && text=="environment.preset name=","debounce publication reset chosen candidate"); break;
    default: break;
    }
    std::printf("Console context smoke stage %u passed\n",stage); return false;
}
}

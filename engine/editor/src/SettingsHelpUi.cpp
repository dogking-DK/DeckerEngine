#include "SettingsHelpUi.hpp"
#include <imgui_internal.h>
#include <algorithm>
#include <cfloat>

namespace dk::editor::detail {
const SettingInfo* SettingsHelpUi::find(std::string_view id) const {
    const auto it=std::ranges::find(entries_,id,&SettingInfo::id); return it==entries_.end() ? nullptr : &*it;
}
void SettingsHelpUi::begin_frame(const CommandCatalogSnapshot& catalog,SettingsContext context,DocumentId document) {
    if(catalog_!=catalog.version) { entries_=build_settings_catalog(catalog); catalog_=catalog.version; }
    if(document_!=document) { pending_.clear(); highlight_.clear(); located_.clear(); focus_target_=false; document_=document; }
    context_=std::move(context); tooltip_.clear(); positions_.clear();
}
void SettingsHelpUi::panel(const char* name) {
    const auto* s=find(pending_);
    if(s && s->panel==name) {
        ImGui::SetNextWindowCollapsed(false);
        if(focus_target_) { ImGui::SetNextWindowFocus(); focus_target_=false; }
    }
}
bool SettingsHelpUi::reveal(std::string_view panel,std::string_view section) const {
    const auto* s=find(pending_); return s && s->panel==panel && s->section.starts_with(section);
}
void SettingsHelpUi::describe(const SettingInfo& s,std::string_view reason) {
    ImGui::TextWrapped("%s / %s / %s",s.panel.c_str(),s.section.c_str(),s.label.c_str());
    ImGui::Separator(); ImGui::TextWrapped("%s",s.explanation.c_str());
    if(!s.help.empty()) ImGui::TextWrapped("%s",s.help.c_str());
    ImGui::TextWrapped("Effect: %s",s.impact.c_str());
    if(!reason.empty()) ImGui::TextWrapped("Unavailable: %.*s",int(reason.size()),reason.data());
    if(s.command.empty()) ImGui::TextDisabled("No command interface for this control.");
}
void SettingsHelpUi::item(std::string_view id,std::string_view reason) {
    const auto* s=find(id); if(!s) return;
    const auto min=ImGui::GetItemRectMin(),max=ImGui::GetItemRectMax();
    positions_[s->id]={(min.x+max.x)/2,(min.y+max.y)/2};
    if(pending_==id) {
        ImGui::SetScrollHereY(.5f); located_=s->id; pending_.clear(); highlight_=s->id; highlight_until_=ImGui::GetTime()+3;
    }
    if(highlight_==id && ImGui::GetTime()<highlight_until_)
        ImGui::GetWindowDrawList()->AddRect({min.x-2,min.y-2},{max.x+2,max.y+2},IM_COL32(255,195,50,255),3.0f,2.0f,ImDrawFlags_None);
    if(ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled|ImGuiHoveredFlags_DelayNormal)) {
        auto unavailable=reason.empty() ? setting_unavailable(*s,context_) : std::string(reason);
        if(unavailable.empty() && (ImGui::GetCurrentContext()->LastItemData.ItemFlags & ImGuiItemFlags_Disabled))
            unavailable="Finish the current interaction or leave the containing disabled state first.";
        tooltip_=s->id+"\n"+unavailable+"\n"+s->help;
        ImGui::BeginTooltip(); ImGui::PushTextWrapPos(ImGui::GetFontSize()*34);
        describe(*s,unavailable); ImGui::PopTextWrapPos(); ImGui::EndTooltip();
    }
}
void SettingsHelpUi::unavailable_panel(const char* panel) {
    const auto* s=find(pending_.empty() ? located_ : pending_); if(!s || s->panel!=panel) return;
    const auto reason=setting_unavailable(*s,context_);
    describe(*s,reason.empty() ? "This control is not currently displayed; finish the active popup or select its view first." : reason);
    item(s->id);
}
void SettingsHelpUi::locate(const SettingInfo& s) {
    pending_=s.id; focus_target_=true; located_.clear(); status_.clear(); open_=false;
}
std::optional<std::string> SettingsHelpUi::draw(bool console_empty,bool composing,std::map<std::string,ImVec2>& controls) {
    if(!open_) return {};
    ImGui::SetNextWindowSize({700,600},ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints({440,420},{FLT_MAX,FLT_MAX});
    if(focus_search_) ImGui::SetNextWindowFocus();
    if(!ImGui::Begin("Settings search",&open_,ImGuiWindowFlags_NoDocking)) { ImGui::End(); return {}; }
    auto mark=[&](const char* key) { const auto a=ImGui::GetItemRectMin(),b=ImGui::GetItemRectMax(); controls[key]={(a.x+b.x)/2,(a.y+b.y)/2}; };
    if(focus_search_) { ImGui::SetKeyboardFocusHere(); focus_search_=false; }
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##settings_query","Search name, parameter or Chinese alias",query_.data(),query_.size()); mark("settings_query");
    const auto results=search_settings(entries_,query_.data());
    if(!results.error.empty()) ImGui::TextWrapped("%s",results.error.c_str());
    ImGui::TextDisabled("%zu result%s%s | Locate preserves values",results.indices.size(),results.indices.size()==1 ? "" : "s",results.truncated ? " (first 64)" : "");
    ImGui::BeginChild("Settings results",{0,175},ImGuiChildFlags_Borders);
    for(const auto i:results.indices) {
        const auto& s=entries_[i]; const auto reason=setting_unavailable(s,context_);
        const auto label=s.label+"  /  "+s.panel+(reason.empty() ? "" : "  (unavailable)")+"##"+s.id;
        if(ImGui::Selectable(label.c_str(),selected_==s.id)) { selected_=s.id; status_.clear(); }
        if(i==results.indices.front()) mark("settings_first");
    }
    ImGui::EndChild();
    if(results.indices.empty()) ImGui::TextUnformatted("No matching controls.");
    const SettingInfo* s=find(selected_);
    if(s && std::ranges::none_of(results.indices,[&](auto i){return entries_[i].id==s->id;})) s=nullptr;
    ImGui::BeginChild("Setting details",{0,-ImGui::GetFrameHeightWithSpacing()*4},ImGuiChildFlags_Borders);
    if(s) {
        describe(*s,setting_unavailable(*s,context_));
        if(!s->command_template.empty() && ImGui::TreeNode("Command template (fill placeholders)")) {
            ImGui::TextWrapped("%s",s->command_template.c_str()); ImGui::TreePop();
        }
    } else ImGui::TextUnformatted("Select a result to see its meaning, constraints and availability.");
    ImGui::EndChild();
    std::optional<std::string> insert;
    ImGui::BeginDisabled(!s);
    if(ImGui::Button("Locate") && s) locate(*s); mark("settings_locate");
    ImGui::SameLine();
    ImGui::BeginDisabled(!s || s->command_template.empty());
    if(ImGui::Button("Copy command") && s) { ImGui::SetClipboardText(s->command_template.c_str()); status_="Copied; fill placeholders before executing."; } mark("settings_copy");
    ImGui::SameLine();
    ImGui::BeginDisabled(!console_empty || composing);
    if(ImGui::Button("Insert into Console") && s) { insert=s->command_template; open_=false; status_="Inserted only; fill placeholders and explicitly Run."; } mark("settings_insert");
    ImGui::EndDisabled(); ImGui::EndDisabled(); ImGui::EndDisabled();
    if(s && !s->command_template.empty() && (!console_empty || composing))
        ImGui::TextWrapped("Insert unavailable: finish IME and clear Console first. Existing input is preserved; Copy remains available.");
    else if(!status_.empty()) ImGui::TextWrapped("%s",status_.c_str());
    ImGui::End(); return insert;
}
}

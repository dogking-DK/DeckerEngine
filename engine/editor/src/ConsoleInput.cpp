#include "ConsoleInput.hpp"
#include <imgui_internal.h>
#include <algorithm>
#include <utility>

namespace dk::editor::detail {
namespace {
// InputText owns Enter and reverts text on Escape. Route already-handled keys
// around this widget only; restore the entire key record immediately afterwards.
class RoutedKeys {
    std::vector<std::pair<ImGuiKey,ImGuiKeyData>> saved_;
public:
    void hide(ImGuiKey key) {
        auto* data=ImGui::GetKeyData(key); saved_.emplace_back(key,*data);
        data->Down=false; data->DownDuration=-1;
    }
    ~RoutedKeys() { for (const auto& [key,data] : saved_) *ImGui::GetKeyData(key)=data; }
};
}
struct ConsoleInput::ClipboardScope {
    ImGuiPlatformIO& platform = ImGui::GetPlatformIO();
    decltype(platform.Platform_GetClipboardTextFn) get = platform.Platform_GetClipboardTextFn;
    decltype(platform.Platform_SetClipboardTextFn) set = platform.Platform_SetClipboardTextFn;
    void* user = platform.Platform_ClipboardUserData;
    ConsoleInput& input;
    explicit ClipboardScope(ConsoleInput& value) : input(value) {
        platform.Platform_GetClipboardTextFn = read;
        platform.Platform_SetClipboardTextFn = write;
        platform.Platform_ClipboardUserData = this;
    }
    ~ClipboardScope() {
        platform.Platform_GetClipboardTextFn = get; platform.Platform_SetClipboardTextFn = set;
        platform.Platform_ClipboardUserData = user;
    }
    struct OriginalUser {
        ClipboardScope& scope;
        explicit OriginalUser(ClipboardScope& value) : scope(value) { scope.platform.Platform_ClipboardUserData=scope.user; }
        ~OriginalUser() { scope.platform.Platform_ClipboardUserData=&scope; }
    };
    static void write(ImGuiContext* ctx, const char* text) {
        auto& scope = *static_cast<ClipboardScope*>(ImGui::GetPlatformIO().Platform_ClipboardUserData);
        OriginalUser original{scope};
        if (scope.set) scope.set(ctx,text);
    }
    static const char* read(ImGuiContext* ctx) {
        auto& platform = ImGui::GetPlatformIO();
        auto& scope = *static_cast<ClipboardScope*>(platform.Platform_ClipboardUserData);
        // Backend callbacks may depend on their original user data.
        OriginalUser original{scope};
        const auto* clipboard = scope.get ? scope.get(ctx) : nullptr;
        if (!clipboard) return nullptr;
        std::size_t bytes = 0;
        while (bytes <= console_input_limit && clipboard[bytes]) ++bytes;
        auto& input = scope.input;
        if (auto problem = console_text_problem({clipboard,bytes})) { input.reject(*problem); return ""; }
        const auto* state = ImGui::GetInputTextState(input.id_);
        const auto selected = state ? std::abs(state->GetSelectionEnd()-state->GetSelectionStart()) : 0;
        const auto current = state ? std::size_t(state->TextLen) : input.text().size();
        if (current - std::size_t(selected) + bytes > console_input_limit) {
            input.reject("Paste exceeds 8192 UTF-8 bytes after replacing selection; input unchanged."); return "";
        }
        input.pasting_ = true;
        // Without AllowTabInput ImGui filters pasted Tabs too. Apply this already
        // validated paste through the callback, preserving every byte/selection.
        if (std::string_view(clipboard,bytes).find('\t')!=std::string_view::npos) {
            input.pending_paste_=std::string(clipboard,bytes); return "";
        }
        return clipboard;
    }
};
bool ConsoleInput::active() const { return id_ && ImGui::GetCurrentContext() && ImGui::GetActiveID() == id_; }
bool ConsoleInput::draw(bool composing, const CommandCatalogSnapshot& catalog) {
    rejected_ = false; pasting_ = false; accepted_=false; accept_key_=false;
    composing_=composing; catalog_=&catalog;
    if (assistance_.catalog!=catalog.version) dismiss();
    id_ = ImGui::GetID("##command");
    if (focus_) { ImGui::SetKeyboardFocusHere(); focus_ = false; }
    if (composing) dismiss();
    if (active() && !composing && popup_ &&
        (ImGui::IsKeyPressed(ImGuiKey_Enter,false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter,false))) accept_key_=true;
    const bool escape=active() && !composing && !hidden_ &&
        (popup_ || !assistance_.hint.empty()) && ImGui::IsKeyPressed(ImGuiKey_Escape,false);
    if (escape) dismiss();
    ClipboardScope clipboard{*this};
    auto flags = ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackEdit |
        ImGuiInputTextFlags_CallbackCharFilter | ImGuiInputTextFlags_CallbackAlways;
    if (!composing) flags |= ImGuiInputTextFlags_CallbackHistory | ImGuiInputTextFlags_CallbackCompletion;
    bool enter=false;
    {
        RoutedKeys routed;
        if (accept_key_ || composing) { routed.hide(ImGuiKey_Enter); routed.hide(ImGuiKey_KeypadEnter); }
        if (escape || composing) routed.hide(ImGuiKey_Escape);
        if (composing) { routed.hide(ImGuiKey_Tab); routed.hide(ImGuiKey_UpArrow); routed.hide(ImGuiKey_DownArrow); }
        enter = ImGui::InputTextWithHint("##command", "scene.query limit=20  (Tab: complete, Up/Down: history)",
            buffer_.data(), buffer_.size(), flags, callback, this);
    }
    input_min_=ImGui::GetItemRectMin(); input_max_=ImGui::GetItemRectMax();
    // A click in our assistance window is handled after the input is drawn.
    const bool in_assistance=GImGui->HoveredWindow && std::string_view(GImGui->HoveredWindow->Name).starts_with("##Console assistance");
    if (!active() && !in_assistance) dismiss();
    catalog_=nullptr;
    return enter && !composing && !rejected_ && !accepted_;
}
void ConsoleInput::sync(ImGuiInputTextCallbackData* data) {
    if (composing_ || !catalog_) return;
    ConsoleEditState current{std::string(data->Buf,std::size_t(data->BufTextLen)),std::size_t(data->CursorPos),
        std::size_t(data->SelectionStart),std::size_t(data->SelectionEnd),edit_.revision};
    if (current==edit_ && assistance_.source==current && assistance_.catalog==catalog_->version) return;
    // Completion has callback priority over Edit when typing and Tab arrive in
    // one frame. Such text changes must still detach history after stale rejection.
    if (current.text!=edit_.text && data->EventFlag!=ImGuiInputTextFlags_CallbackHistory) history_.edited();
    if (current!=edit_) ++current.revision;
    edit_=std::move(current);
    assistance_=assist_console(*catalog_,edit_); selected_=0; scroll_candidate_=true; hidden_=false;
    if (assistance_.candidates.empty()) popup_=false;
}
void ConsoleInput::accept(ImGuiInputTextCallbackData* data) {
    ConsoleEditState current{std::string(data->Buf,std::size_t(data->BufTextLen)),std::size_t(data->CursorPos),
        std::size_t(data->SelectionStart),std::size_t(data->SelectionEnd),edit_.revision};
    auto replacement=accept_console_candidate(assistance_,selected_,current,catalog_->version);
    dismiss();
    if (!replacement) { reject(replacement.error().message); return; }
    data->DeleteChars(0,data->BufTextLen); data->InsertChars(0,replacement->text.c_str());
    data->CursorPos=data->SelectionStart=data->SelectionEnd=int(replacement->cursor);
    edit_=std::move(*replacement); history_.edited(); accepted_=true;
}
int ConsoleInput::callback(ImGuiInputTextCallbackData* data) {
    auto& input = *static_cast<ConsoleInput*>(data->UserData);
    if (data->EventFlag!=ImGuiInputTextFlags_CallbackCharFilter && input.pending_paste_) {
        const auto begin=std::min(data->SelectionStart,data->SelectionEnd),end=std::max(data->SelectionStart,data->SelectionEnd);
        data->DeleteChars(begin,end-begin); data->InsertChars(begin,input.pending_paste_->c_str());
        data->CursorPos=data->SelectionStart=data->SelectionEnd=begin+int(input.pending_paste_->size());
        input.pending_paste_.reset(); input.history_.edited();
    }
    if (data->EventFlag == ImGuiInputTextFlags_CallbackCharFilter) {
        if (!input.pasting_) {
            const auto* state = ImGui::GetInputTextState(data->ID);
            const auto c = data->EventChar;
            const auto bytes = c < 0x80 ? 1 : c < 0x800 ? 2 : c < 0x10000 ? 3 : 4;
            if (state && state->TextLen - std::abs(data->SelectionEnd-data->SelectionStart) + bytes > int(console_input_limit)) {
                input.reject("Console input exceeds 8192 UTF-8 bytes; character rejected."); return 1;
            }
        }
    } else if (data->EventFlag == ImGuiInputTextFlags_CallbackHistory) {
        if (input.popup_) {
            input.sync(data);
            if (!input.popup_) return 0;
            if (data->EventKey==ImGuiKey_UpArrow && input.selected_) --input.selected_;
            if (data->EventKey==ImGuiKey_DownArrow && input.selected_+1<input.assistance_.candidates.size()) ++input.selected_;
            input.scroll_candidate_=true; return 0;
        }
        const std::string_view current{data->Buf,std::size_t(data->BufTextLen)};
        // Copy before editing because next() may return the current input itself.
        const std::string replacement{data->EventKey == ImGuiKey_UpArrow ? input.history_.previous(current) : input.history_.next(current)};
        data->DeleteChars(0,data->BufTextLen); data->InsertChars(0,replacement.c_str());
    } else if (data->EventFlag == ImGuiInputTextFlags_CallbackCompletion) {
        if (input.popup_) input.accept(data);
        else { input.sync(data); input.popup_=!input.assistance_.candidates.empty(); input.hidden_=false; input.scroll_candidate_=true; }
    } else if (data->EventFlag == ImGuiInputTextFlags_CallbackEdit) input.history_.edited();
    if (data->EventFlag!=ImGuiInputTextFlags_CallbackCharFilter) {
        if (input.accept_key_) { input.accept_key_=false; input.accept(data); }
        else if (!input.accepted_) input.sync(data);
    }
    return 0;
}
void ConsoleInput::draw_assistance() {
    positions_.clear();
    if (composing_ || hidden_ || (!popup_ && (!active() || assistance_.hint.empty()))) return;
    const auto* viewport=ImGui::GetMainViewport();
    const auto width=std::min(640.0f,viewport->WorkSize.x-16.0f);
    const auto height=std::min(380.0f,viewport->WorkSize.y-16.0f);
    ImGui::SetNextWindowPos({std::clamp(input_min_.x,viewport->WorkPos.x,viewport->WorkPos.x+viewport->WorkSize.x-width),
        std::max(viewport->WorkPos.y,input_min_.y-height-4)});
    ImGui::SetNextWindowSize({width,height});
    const auto flags=ImGuiWindowFlags_NoTitleBar|ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoResize|
        ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_NoFocusOnAppearing|ImGuiWindowFlags_NoNav;
    ImGui::Begin("##Console assistance",nullptr,flags);
    if (popup_) {
        ImGui::TextUnformatted("Tab / Enter: accept   Esc: close   Up / Down: choose");
        if (assistance_.truncated) ImGui::TextUnformatted("Showing 32 matches; type a longer prefix.");
        const float rows=float(std::min(std::size_t(8),assistance_.candidates.size()));
        ImGui::BeginChild("##candidates",{0,rows*ImGui::GetTextLineHeightWithSpacing()+8},true);
        for (std::size_t i=0;i<assistance_.candidates.size();++i) {
            ImGui::PushID(int(i));
            const auto label_pos=ImGui::GetCursorScreenPos();
            const bool chosen=ImGui::Selectable("##candidate",selected_==i,
                ImGuiSelectableFlags_SelectOnClick|ImGuiSelectableFlags_NoHoldingActiveID);
            ImGui::GetWindowDrawList()->AddText(label_pos,ImGui::GetColorU32(ImGuiCol_Text),assistance_.candidates[i].label.c_str());
            const auto a=ImGui::GetItemRectMin(),b=ImGui::GetItemRectMax(); positions_.push_back({(a.x+b.x)*.5f,(a.y+b.y)*.5f});
            if (selected_==i && scroll_candidate_) ImGui::SetScrollHereY();
            ImGui::PopID();
            if (chosen) {
                const auto replacement=accept_console_candidate(assistance_,i,edit_,assistance_.catalog);
                dismiss();
                if (!replacement) reject(replacement.error().message);
                else {
                    edit_=*replacement; std::copy(edit_.text.begin(),edit_.text.end(),buffer_.begin()); buffer_[edit_.text.size()]=0;
                    if (auto* state=ImGui::GetInputTextState(id_)) {
                        state->ReloadUserBufAndKeepSelection(); state->ReloadSelectionStart=state->ReloadSelectionEnd=int(edit_.cursor);
                    }
                    history_.edited(); accepted_=true; focus_=true;
                }
                break;
            }
        }
        scroll_candidate_=false; ImGui::EndChild();
    }
    const auto& hint=popup_ && !assistance_.candidates.empty() ? assistance_.candidates[selected_].description : assistance_.hint;
    ImGui::TextWrapped("%s",hint.c_str());
    ImGui::End();
}
void ConsoleInput::clear() {
    buffer_[0] = '\0'; history_.edited(); dismiss();
    if (auto* state = ImGui::GetInputTextState(id_)) state->ReloadUserBufAndMoveToEnd();
}
} // namespace dk::editor::detail

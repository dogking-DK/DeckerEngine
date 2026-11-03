#include "ConsoleInput.hpp"
#include <imgui_internal.h>
#include <algorithm>
#include <utility>

namespace dk::editor::detail {
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
        return clipboard;
    }
};
bool ConsoleInput::active() const { return id_ && ImGui::GetCurrentContext() && ImGui::GetActiveID() == id_; }
bool ConsoleInput::draw(bool composing) {
    rejected_ = false; pasting_ = false;
    id_ = ImGui::GetID("##command");
    if (focus_) { ImGui::SetKeyboardFocusHere(); focus_ = false; }
    ClipboardScope clipboard{*this};
    auto flags = ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackEdit |
        ImGuiInputTextFlags_CallbackCharFilter | ImGuiInputTextFlags_AllowTabInput;
    if (!composing) flags |= ImGuiInputTextFlags_CallbackHistory;
    const bool enter = ImGui::InputTextWithHint("##command", "scene.query limit=20  (Up/Down: history)",
        buffer_.data(), buffer_.size(), flags, callback, this);
    return enter && !composing && !rejected_;
}
int ConsoleInput::callback(ImGuiInputTextCallbackData* data) {
    auto& input = *static_cast<ConsoleInput*>(data->UserData);
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
        const std::string_view current{data->Buf,std::size_t(data->BufTextLen)};
        // Copy before editing because next() may return the current input itself.
        const std::string replacement{data->EventKey == ImGuiKey_UpArrow ? input.history_.previous(current) : input.history_.next(current)};
        data->DeleteChars(0,data->BufTextLen); data->InsertChars(0,replacement.c_str());
    } else if (data->EventFlag == ImGuiInputTextFlags_CallbackEdit) input.history_.edited();
    return 0;
}
void ConsoleInput::clear() {
    buffer_[0] = '\0'; history_.edited();
    if (auto* state = ImGui::GetInputTextState(id_)) state->ReloadUserBufAndMoveToEnd();
}
} // namespace dk::editor::detail

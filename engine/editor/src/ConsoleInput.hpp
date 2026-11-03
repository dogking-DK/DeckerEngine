#pragma once
#include "ConsoleHistory.hpp"
#include "ConsoleAssistCache.hpp"
#include <imgui.h>
#include <array>
#include <utility>

namespace dk::editor::detail {
class ConsoleInput final {
public:
    bool draw(bool composing, const CommandCatalogSnapshot&, const Workspace&);
    void draw_assistance();
    void dismiss() { popup_ = false; hidden_ = true; }
    bool remember() { return history_.remember(text()); }
    void clear();
    void focus() { focus_ = true; }
    [[nodiscard]] bool active() const;
    [[nodiscard]] std::string_view text() const { return buffer_.data(); }
    [[nodiscard]] std::size_t history_size() const { return history_.size(); }
    [[nodiscard]] bool rejected() const { return rejected_; }
    [[nodiscard]] bool accepted() const { return accepted_; }
    [[nodiscard]] bool popup_open() const { return popup_; }
    [[nodiscard]] std::size_t selected_candidate() const { return selected_; }
    [[nodiscard]] const ConsoleAssistance& assistance() const { return assistance_; }
    [[nodiscard]] const std::vector<ImVec2>& candidate_positions() const { return positions_; }
    std::string take_error() { return std::exchange(error_, {}); }
private:
    struct ClipboardScope;
    static int callback(ImGuiInputTextCallbackData*);
    void sync(ImGuiInputTextCallbackData*,bool explicit_tab=false);
    void accept(ImGuiInputTextCallbackData*);
    void reject(std::string error) { rejected_ = true; error_ = std::move(error); }
    std::array<char,console_input_limit+1> buffer_{};
    ConsoleHistory history_;
    ImGuiID id_ = 0;
    bool focus_ = false, rejected_ = false, pasting_ = false;
    std::string error_;
    std::optional<std::string> pending_paste_;
    const CommandCatalogSnapshot* catalog_ = nullptr; // only borrowed during draw/callback
    const Workspace* workspace_ = nullptr;
    ConsoleReferenceVersion references_;
    ConsoleAssistCache cache_;
    std::uint64_t displayed_publication_=0;
    ConsoleEditState edit_;
    ConsoleAssistance assistance_;
    std::size_t selected_ = 0;
    bool popup_ = false, hidden_ = false, composing_ = false, accept_key_ = false, accepted_ = false, scroll_candidate_ = false;
    ImVec2 input_min_{}, input_max_{};
    std::vector<ImVec2> positions_;
};
} // namespace dk::editor::detail

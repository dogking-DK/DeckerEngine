#include "ConsoleHistory.hpp"

namespace dk::editor::detail {
std::optional<std::string> console_text_problem(std::string_view text) {
    if (text.size() > console_input_limit) return "Console input exceeds 8192 UTF-8 bytes; input unchanged.";
    for (unsigned char c : text)
        if (c < 0x20 && c != '\t') return "Console accepts one line without control bytes; input unchanged.";
    if (!validate_command_value(Json(std::string{text}), 1024 * 1024)) return "Invalid UTF-8; input unchanged.";
    return {};
}
std::string console_log_text(std::string_view text) {
    constexpr std::size_t limit = 4096;
    constexpr std::string_view suffix = " ... (truncated; use IPC for full result)";
    if (text.size() <= limit) return std::string{text};
    auto end = limit - suffix.size();
    while (end && (static_cast<unsigned char>(text[end]) & 0xc0) == 0x80) --end;
    return std::string{text.substr(0,end)} + std::string{suffix};
}
bool ConsoleHistory::remember(std::string_view text) {
    if (console_text_problem(text)) return false;
    const auto start = text.find_first_not_of(" \t");
    if (start == std::string_view::npos) return false;
    text = text.substr(start, text.find_last_not_of(" \t") - start + 1);
    if (entries_.empty() || entries_.back() != text) {
        std::string entry{text};
        entries_.push_back(std::move(entry));
        if (entries_.size() > capacity) entries_.erase(entries_.begin());
    }
    edited();
    return true;
}
std::string_view ConsoleHistory::previous(std::string_view current) {
    if (entries_.empty() || console_text_problem(current)) return current;
    if (!cursor_) { draft_ = current; cursor_ = entries_.size()-1; }
    else if (*cursor_) --*cursor_;
    return entries_[*cursor_];
}
std::string_view ConsoleHistory::next(std::string_view current) {
    if (!cursor_) return current;
    if (*cursor_+1 < entries_.size()) return entries_[++*cursor_];
    cursor_.reset();
    return draft_;
}
} // namespace dk::editor::detail

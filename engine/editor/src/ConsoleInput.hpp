#pragma once
#include "ConsoleHistory.hpp"
#include <imgui.h>
#include <array>
#include <utility>

namespace dk::editor::detail {
class ConsoleInput final {
public:
    bool draw(bool composing);
    bool remember() { return history_.remember(text()); }
    void clear();
    void focus() { focus_ = true; }
    [[nodiscard]] bool active() const;
    [[nodiscard]] std::string_view text() const { return buffer_.data(); }
    [[nodiscard]] std::size_t history_size() const { return history_.size(); }
    [[nodiscard]] bool rejected() const { return rejected_; }
    std::string take_error() { return std::exchange(error_, {}); }
private:
    struct ClipboardScope;
    static int callback(ImGuiInputTextCallbackData*);
    void reject(std::string error) { rejected_ = true; error_ = std::move(error); }
    std::array<char,console_input_limit+1> buffer_{};
    ConsoleHistory history_;
    ImGuiID id_ = 0;
    bool focus_ = false, rejected_ = false, pasting_ = false;
    std::string error_;
};
} // namespace dk::editor::detail

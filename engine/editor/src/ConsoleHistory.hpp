#pragma once
#include <dk/editor/Console.hpp>
#include <optional>
#include <vector>

namespace dk::editor::detail {
[[nodiscard]] std::optional<std::string> console_text_problem(std::string_view);
[[nodiscard]] std::string console_log_text(std::string_view);
class ConsoleHistory final {
public:
    static constexpr std::size_t capacity = 64;
    // Store submission attempts, not execution results. Blank/invalid input is ignored.
    bool remember(std::string_view);
    std::string_view previous(std::string_view current);
    std::string_view next(std::string_view current);
    void edited() { cursor_.reset(); draft_.clear(); }
    [[nodiscard]] std::size_t size() const { return entries_.size(); }
    [[nodiscard]] const std::vector<std::string>& entries() const { return entries_; }
private:
    std::vector<std::string> entries_;
    std::optional<std::size_t> cursor_;
    std::string draft_;
};
} // namespace dk::editor::detail

#pragma once
#include <string_view>

namespace dk::editor::detail {
inline bool space(char c) { return c == ' ' || c == '\t'; }
inline bool letter(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
inline bool digit(char c) { return c >= '0' && c <= '9'; }
inline bool name_char(char c) { return letter(c) || digit(c) || c == '_'; }
inline bool bare_forbidden(char c) { return std::string_view{"\"'[]{}=;&|"}.find(c) != std::string_view::npos; }
}

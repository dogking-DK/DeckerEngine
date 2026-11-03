#pragma once
#include <dk/editor/Console.hpp>
#include <vector>

namespace dk::editor::detail {
enum class ConsoleDiagnosticKind { incomplete, error };
struct ConsoleDiagnostic {
    ConsoleDiagnosticKind kind;
    std::size_t byte = 0, column = 1;
    std::string message;
};
enum class ConsoleShape { missing, scalar, object, array };
struct ConsoleNode {
    std::string key;
    std::size_t key_begin=0, key_end=0, begin=0, end=0;
    bool has_key=false, key_complete=false, separator=false, complete=false, valid=true;
    ConsoleShape shape=ConsoleShape::missing;
    Json value;
    std::vector<ConsoleNode> children;
};
struct ConsoleStructure {
    bool envelope=false;
    std::string method;
    std::size_t method_begin=0, method_end=0;
    ConsoleNode root;
    std::vector<ConsoleDiagnostic> diagnostics;
};
[[nodiscard]] ConsoleStructure scan_console_structure(std::string_view, std::size_t cursor);
void console_diagnostic(std::vector<ConsoleDiagnostic>&, std::string_view, ConsoleDiagnosticKind, std::size_t, std::string);
}

#pragma once
#include <dk/commands/CommandRegistry.hpp>
namespace dk::editor::detail {
// Shared, bounded schema/help presentation for Console and panel assistance.
std::string command_field_help(const CommandDescriptor*,const std::string& path,const Json&,bool required);
}

#pragma once
#include <dk/operations/EditorViewportOperations.hpp>
namespace dk {
struct EditorCommandEntry { CommandDescriptor descriptor; CommandHandler handler; };
using EditorHost=std::function<std::shared_ptr<EditorViewportService>()>;
std::vector<EditorCommandEntry> editor_camera_commands(EditorHost,const Json& viewport_result);
}

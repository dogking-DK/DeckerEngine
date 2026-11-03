#pragma once
#include <dk/commands/CommandRegistry.hpp>
#include <dk/services/EditorViewportService.hpp>
namespace dk {
Result<void> register_editor_viewport_commands(CommandRegistry&,std::function<std::shared_ptr<EditorViewportService>()>);
Json editor_viewport_state_json(const EditorViewportState&);
}

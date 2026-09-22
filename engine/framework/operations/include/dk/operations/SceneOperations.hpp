#pragma once
#include <dk/commands/CommandRegistry.hpp>
#include <dk/services/SceneService.hpp>

namespace dk
{
[[nodiscard]] Json document_state_schema();
[[nodiscard]] Json document_state_json(DocumentState state);
[[nodiscard]] Json edit_guard_json(EditGuard guard);
[[nodiscard]] Result<EditGuard> parse_edit_guard(const Json &value);
// Requires parameters already checked against the registered edit schema.
[[nodiscard]] Result<SceneEdit> decode_scene_edit(std::string_view method, const Json &parameters);
[[nodiscard]] Result<void> register_scene_commands(CommandRegistry &registry, SceneService &service);
} // namespace dk

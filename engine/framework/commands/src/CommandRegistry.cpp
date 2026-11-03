#include <dk/commands/CommandRegistry.hpp>
#include "CommandHelp.hpp"
#include <magic_enum/magic_enum.hpp>
#include <limits>
#include <stdexcept>

namespace dk {
namespace {
bool valid_name(std::string_view name) {
    const auto letter = [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); };
    if (name.empty() || name.size()>96 || !letter(name.front())) return false;
    for (char c : name) if (!letter(c) && !(c >= '0' && c <= '9') && c!='.' && c!='_') return false;
    return true;
}
}
std::string_view effect_name(CommandEffect effect) noexcept {
    const auto name = magic_enum::enum_name(effect);
    return name.empty() ? "invalid" : name;
}
CommandRegistry::CommandRegistry() {
    auto id = CommandCatalogId::generate();
    if (!id) throw std::runtime_error(id.error().message);
    version_.registry_id = *id;
    auto r = add({"commands.list", "List registered commands", schema::object(), schema::array(Json::object())}, [this](const Json&) -> Result<Json> { return list(); });
    if (!r) throw std::logic_error(r.error().message);
    r = add({"commands.describe", "Describe a command and its schemas", schema::object({{"name", schema::string(1, 96)}}, {"name"}), {{"type", "object"}}, CommandEffect::query, false,
        Json{{"version",1},{"fields",{{"/properties/name",{{"label","命令名"},{"description","Exact registered command name; inspect commands.list for available commands."},
            {"examples",Json::array({"commands.list","commands.describe"})},{"aliases",Json::array({"命令","command"})}}}}}}},
        [this](const Json& p) { return describe(p.at("name").get<std::string>()); });
    if (!r) throw std::logic_error(r.error().message);
}
Result<void> CommandRegistry::add(CommandDescriptor descriptor, CommandHandler handler) {
    if (!valid_name(descriptor.name) || descriptor.description.empty() || !handler || effect_name(descriptor.effect) == "invalid" || (descriptor.undoable && descriptor.effect != CommandEffect::memory_edit)) return std::unexpected(Error{ErrorCode::invalid_argument, "Invalid command descriptor or handler"});
    for (const auto* definition : {&descriptor.parameters, &descriptor.result}) { auto r = schema::check(*definition); if (!r) return r; }
    auto text = validate_command_value(Json(descriptor.description), 1024 * 1024); if (!text) return text;
    if (descriptor.help) {
        auto valid = detail::validate_command_help(descriptor.parameters,*descriptor.help,completion_providers_);
        if (!valid) return std::unexpected(valid.error().with_context(descriptor.name));
    }
    if (entries_.contains(descriptor.name)) return std::unexpected(Error{ErrorCode::invalid_argument, "Command already registered: " + descriptor.name});
    if (version_.revision==std::numeric_limits<std::uint64_t>::max()) return std::unexpected(Error{ErrorCode::invalid_state,"Command catalog revision exhausted"});
    auto name = descriptor.name;
    entries_.emplace(std::move(name), Entry{std::move(descriptor), std::move(handler)});
    ++version_.revision;
    return {};
}
Result<void> CommandRegistry::register_completion_provider(std::string id) {
    if (!valid_name(id) || completion_providers_.contains(id))
        return std::unexpected(Error{ErrorCode::invalid_argument,"Invalid or duplicate completion provider: " + id});
    if (version_.revision==std::numeric_limits<std::uint64_t>::max()) return std::unexpected(Error{ErrorCode::invalid_state,"Command catalog revision exhausted"});
    completion_providers_.insert(std::move(id)); ++version_.revision;
    return {};
}
CommandCatalogSnapshot CommandRegistry::catalog_snapshot() const {
    CommandCatalogSnapshot snapshot{version_,{}, {completion_providers_.begin(),completion_providers_.end()}};
    snapshot.commands.reserve(entries_.size());
    for (const auto& [name,entry] : entries_) snapshot.commands.push_back(entry.descriptor);
    return snapshot;
}
Result<void> CommandRegistry::validate_parameters(std::string_view name, const Json& parameters) const {
    const auto it = entries_.find(name);
    if (it == entries_.end()) return std::unexpected(Error{ErrorCode::not_found, "Unknown command: " + std::string(name)});
    auto r = validate_command_value(parameters, 1024 * 1024); if (!r) return r;
    return schema::validate(it->second.descriptor.parameters, parameters);
}
Result<Json> CommandRegistry::execute(std::string_view name, const Json& parameters) const {
    auto valid = validate_parameters(name, parameters);
    if (!valid) return std::unexpected(valid.error().with_context(std::string(name)));
    const auto& entry = entries_.find(name)->second;
    try {
        auto result = entry.handler(parameters);
        if (!result) return std::unexpected(result.error().with_context(std::string(name)));
        valid = schema::validate(entry.descriptor.result, *result);
        if (!valid) return std::unexpected(Error{ErrorCode::internal_error, "Handler returned invalid result", {valid.error().message, std::string(name)}});
        return result;
    } catch (const std::bad_alloc&) { throw; }
      catch (const std::exception& e) { return std::unexpected(Error{ErrorCode::internal_error, e.what(), {std::string(name)}}); }
      catch (...) { return std::unexpected(Error{ErrorCode::internal_error, "Unknown handler exception", {std::string(name)}}); }
}
Json CommandRegistry::list() const {
    Json result = Json::array();
    for (const auto& [name, entry] : entries_) { const auto& d = entry.descriptor; result.push_back({{"name", name}, {"description", d.description}, {"effect", effect_name(d.effect)}, {"undoable", d.undoable}}); }
    return result;
}
Result<Json> CommandRegistry::describe(std::string_view name) const {
    const auto it = entries_.find(name);
    if (it == entries_.end()) return std::unexpected(Error{ErrorCode::not_found, "Unknown command: " + std::string(name)});
    const auto& d = it->second.descriptor;
    Json result{{"name", d.name}, {"description", d.description}, {"parameters", d.parameters}, {"result", d.result}, {"effect", effect_name(d.effect)}, {"undoable", d.undoable}};
    if (d.help) result["help"] = *d.help;
    return result;
}
} // namespace dk

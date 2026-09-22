#pragma once
#include <dk/commands/Schema.hpp>
#include <functional>
#include <map>

namespace dk {
enum class CommandEffect { query, memory_edit, external, control };
[[nodiscard]] std::string_view effect_name(CommandEffect effect) noexcept;
struct CommandDescriptor {
    std::string name;
    std::string description;
    Json parameters;
    Json result;
    CommandEffect effect = CommandEffect::query;
    bool undoable = false;
};
using CommandHandler = std::function<Result<Json>(const Json&)>;
class CommandRegistry final {
public:
    CommandRegistry();
    CommandRegistry(const CommandRegistry&) = delete;
    CommandRegistry& operator=(const CommandRegistry&) = delete;
    CommandRegistry(CommandRegistry&&) = delete;
    CommandRegistry& operator=(CommandRegistry&&) = delete;
    [[nodiscard]] Result<void> add(CommandDescriptor descriptor, CommandHandler handler);
    [[nodiscard]] Result<void> validate_parameters(std::string_view name, const Json& parameters) const;
    [[nodiscard]] Result<Json> execute(std::string_view name, const Json& parameters = Json::object()) const;
    [[nodiscard]] Result<Json> describe(std::string_view name) const;
    [[nodiscard]] Json list() const;
private:
    struct Entry { CommandDescriptor descriptor; CommandHandler handler; };
    std::map<std::string, Entry, std::less<>> entries_;
};
} // namespace dk

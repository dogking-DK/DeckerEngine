#pragma once
#include <dk/commands/Schema.hpp>
#include <dk/core/StableId.hpp>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <vector>

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
    std::optional<Json> help;
};
struct CommandCatalogIdTag;
using CommandCatalogId = StableId<CommandCatalogIdTag>;
struct CommandCatalogVersion {
    CommandCatalogId registry_id;
    std::uint64_t revision = 0;
    auto operator<=>(const CommandCatalogVersion&) const = default;
};
// Owning metadata only: no handlers or references into the registry/services.
struct CommandCatalogSnapshot {
    CommandCatalogVersion version;
    std::vector<CommandDescriptor> commands;
    std::vector<std::string> completion_providers;
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
    // Declares a semantic ID; executing a provider is the host's responsibility.
    [[nodiscard]] Result<void> register_completion_provider(std::string id);
    // Owner-thread reads. Neither API executes a command or records a task.
    [[nodiscard]] CommandCatalogVersion catalog_version() const { return version_; }
    [[nodiscard]] CommandCatalogSnapshot catalog_snapshot() const;
    [[nodiscard]] Result<void> validate_parameters(std::string_view name, const Json& parameters) const;
    [[nodiscard]] Result<Json> execute(std::string_view name, const Json& parameters = Json::object()) const;
    [[nodiscard]] Result<Json> describe(std::string_view name) const;
    [[nodiscard]] Json list() const;
private:
    struct Entry { CommandDescriptor descriptor; CommandHandler handler; };
    std::map<std::string, Entry, std::less<>> entries_;
    std::set<std::string, std::less<>> completion_providers_;
    CommandCatalogVersion version_;
};
} // namespace dk

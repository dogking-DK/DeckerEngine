#pragma once
#include <dk/commands/CommandRegistry.hpp>
#include <dk/services/SceneService.hpp>

namespace dk
{
struct TaskIdTag;
using TaskId = StableId<TaskIdTag>;
struct CommandExecution
{
    TaskId task_id;
    Result<Json> result;
};
struct TaskRecord
{
    TaskId id;
    std::string method;
    bool succeeded;
    std::optional<ErrorCode> error_code;
    std::optional<DocumentState> document;
};
class Runtime final
{
  public:
    [[nodiscard]] static Result<std::unique_ptr<Runtime>> create(const std::filesystem::path &root);
    [[nodiscard]] Result<CommandExecution> dispatch(std::string_view method, const Json &parameters,
                                                    bool auto_guard = false);
    [[nodiscard]] bool has_command(std::string_view method) const;
    [[nodiscard]] bool stopping() const noexcept
    {
        return stopping_;
    }

  private:
    explicit Runtime(std::unique_ptr<SceneService> service) : service_{std::move(service)} {}
    std::unique_ptr<SceneService> service_;
    CommandRegistry commands_;
    bool dispatching_ = false;
    bool stopping_ = false;
    std::vector<TaskRecord> tasks_;
    [[nodiscard]] Result<void> register_runtime_commands();
};
} // namespace dk

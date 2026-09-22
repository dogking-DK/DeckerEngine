#include <dk/operations/SceneOperations.hpp>
#include <dk/runtime/Runtime.hpp>

namespace dk
{
namespace
{
Json task_schema()
{
    return schema::object({{"id", schema::string(36, 36)},
                           {"method", schema::string(1, 96)},
                           {"status", {{"type", "string"}, {"enum", {"succeeded", "failed"}}}},
                           {"error_code", schema::nullable(schema::integer())},
                           {"document", schema::nullable(document_state_schema())}},
                          {"id", "method", "status", "error_code", "document"});
}
Json task_json(const TaskRecord &task)
{
    return {{"id", task.id.to_string()},
            {"method", task.method},
            {"status", task.succeeded ? "succeeded" : "failed"},
            {"error_code", task.error_code ? Json(static_cast<unsigned>(*task.error_code)) : Json(nullptr)},
            {"document", task.document ? document_state_json(*task.document) : Json(nullptr)}};
}
} // namespace
Result<std::unique_ptr<Runtime>> Runtime::create(const std::filesystem::path &root)
{
    auto service = SceneService::create(root);
    if (!service)
        return std::unexpected(service.error());
    auto runtime = std::unique_ptr<Runtime>(new Runtime(std::move(*service)));
    auto registered = register_scene_commands(runtime->commands_, *runtime->service_);
    if (!registered)
        return std::unexpected(registered.error());
    runtime->tasks_.reserve(257);
    registered = runtime->register_runtime_commands();
    if (!registered)
        return std::unexpected(registered.error());
    return runtime;
}
Result<void> Runtime::register_runtime_commands()
{
    auto r = commands_.add({"runtime.capabilities", "Describe synchronous CPU runtime capabilities",
                            schema::object(),
                            schema::object({{"protocol", schema::string()},
                                            {"async_tasks", schema::boolean()},
                                            {"task_retention", schema::integer()},
                                            {"max_line_bytes", schema::integer()},
                                            {"max_batch_requests", schema::integer()},
                                            {"transactions", schema::boolean()},
                                            {"guard", schema::string()}},
                                           {"protocol", "async_tasks", "task_retention", "max_line_bytes",
                                            "max_batch_requests", "transactions", "guard"})},
                           [](const Json &) -> Result<Json>
                           {
                               return Json{{"protocol", "jsonrpc-2.0-jsonl"}, {"async_tasks", false},
                                           {"task_retention", 256},           {"max_line_bytes", 1024 * 1024},
                                           {"max_batch_requests", 128},       {"transactions", true},
                                           {"guard", "document_id+revision"}};
                           });
    if (!r)
        return r;
    r = commands_.add({"tasks.list", "List retained completed tasks in completion order", schema::object(),
                       schema::array(task_schema(), 0, 256)},
                      [this](const Json &) -> Result<Json>
                      {
                          auto result = Json::array();
                          for (const auto &task : tasks_)
                              result.push_back(task_json(task));
                          return result;
                      });
    if (!r)
        return r;
    r = commands_.add(
        {"tasks.get", "Get a retained completed task by its UUID",
         schema::object({{"id", schema::string(36, 36)}}, {"id"}), task_schema()},
        [this](const Json &p) -> Result<Json>
        {
            auto id = TaskId::parse(p["id"].get<std::string>());
            if (!id)
                return std::unexpected(id.error());
            if (id->is_nil())
                return std::unexpected(Error{ErrorCode::invalid_argument, "Task ID must not be nil"});
            for (const auto &task : tasks_)
                if (task.id == *id)
                    return task_json(task);
            return std::unexpected(Error{ErrorCode::not_found, "Task is unknown or no longer retained"});
        });
    if (!r)
        return r;
    return commands_.add({"runtime.shutdown", "Stop after flushing the current response", schema::object(),
                          schema::object({{"stopping", schema::boolean()}}, {"stopping"}),
                          CommandEffect::control, false},
                         [this](const Json &) -> Result<Json>
                         {
                             Json value{{"stopping", true}};
                             stopping_ = true;
                             return value;
                         });
}
bool Runtime::has_command(std::string_view method) const
{
    return commands_.describe(method).has_value();
}
Result<CommandExecution> Runtime::dispatch(std::string_view method, const Json &parameters, bool auto_guard)
{
    if (stopping_)
        return std::unexpected(Error{ErrorCode::invalid_state, "Runtime is stopping"});
    if (!has_command(method))
        return std::unexpected(Error{ErrorCode::not_found, "Unknown command"});
    if (!parameters.is_object())
        return std::unexpected(Error{ErrorCode::invalid_argument, "Named object parameters required"});
    if (dispatching_)
        return std::unexpected(Error{ErrorCode::invalid_state, "Runtime dispatch is not reentrant"});
    dispatching_ = true;
    struct Reset
    {
        bool &flag;
        ~Reset()
        {
            flag = false;
        }
    } reset{dispatching_};
    auto effective = parameters;
    if (auto_guard && effective.is_object() && !effective.contains("guard"))
    {
        auto descriptor = commands_.describe(method);
        if (descriptor && (*descriptor)["parameters"].contains("properties") &&
            (*descriptor)["parameters"]["properties"].contains("guard"))
        {
            auto state = service_->state();
            if (state)
                effective["guard"] = edit_guard_json({state->document_id, state->revision});
        }
    }
    auto id = TaskId::generate();
    if (!id)
        return std::unexpected(id.error());
    TaskRecord task{*id, std::string(method), false, {}, {}};
    auto result = commands_.execute(method, effective);
    task.succeeded = result.has_value();
    if (!result)
        task.error_code = result.error().code;
    if (auto state = service_->state(); state)
        task.document = *state;
    tasks_.push_back(std::move(task));
    if (tasks_.size() > 256)
        tasks_.erase(tasks_.begin());
    return CommandExecution{*id, std::move(result)};
}
} // namespace dk

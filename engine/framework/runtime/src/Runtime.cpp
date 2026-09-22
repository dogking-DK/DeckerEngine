#include <dk/operations/SceneOperations.hpp>
#include <dk/runtime/Runtime.hpp>

namespace dk
{
Result<std::unique_ptr<Runtime>> Runtime::create(const std::filesystem::path &root)
{
    auto service = SceneService::create(root);
    if (!service)
        return std::unexpected(service.error());
    auto runtime = std::unique_ptr<Runtime>(new Runtime(std::move(*service)));
    auto registered = register_scene_commands(runtime->commands_, *runtime->service_);
    if (!registered)
        return std::unexpected(registered.error());
    return runtime;
}
bool Runtime::has_command(std::string_view method) const
{
    return commands_.describe(method).has_value();
}
Result<Json> Runtime::dispatch(std::string_view method, const Json &parameters, bool auto_guard)
{
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
    return commands_.execute(method, effective);
}
} // namespace dk

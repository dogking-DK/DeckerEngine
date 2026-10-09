#include <dk/operations/SceneOperations.hpp>
#include <dk/operations/SimulationOperations.hpp>
#include <dk/runtime/Runtime.hpp>
#include <dk/profiling/Profiler.hpp>
#ifdef DK_RUNTIME_ASSETS
#include <dk/operations/AssetOperations.hpp>
#endif

#ifdef DK_RUNTIME_CAPTURE
#include <dk/operations/RenderOperations.hpp>
#endif
#if defined(DK_RUNTIME_CAPTURE) || defined(DK_RUNTIME_ASSETS)
#include <dk/operations/JobOperations.hpp>
#endif

namespace dk
{
Result<SceneReadSnapshot> Runtime::read_scene(EditGuard guard) const
{
    return service_->read_snapshot(guard);
}
Result<PlaySceneSnapshot> Runtime::read_play_scene(SimulationId id) const
{
    return simulation_.read_snapshot(id);
}
Result<PlayParticleSnapshot> Runtime::read_play_particles(SimulationId id) const
{
    return simulation_.read_particles(id);
}
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
    DK_PROFILE_ZONE("Runtime.Create");
    auto service = SceneService::create(root);
    if (!service)
        return std::unexpected(service.error());
    auto runtime = std::unique_ptr<Runtime>(new Runtime(std::move(*service)));
#ifdef DK_RUNTIME_ASSETS
    auto assets = AsyncAssetService::create(root, [events = runtime->events_] { events->notify(); });
    if (!assets) return std::unexpected(assets.error());
    runtime->assets_ = std::move(*assets);
    auto asset_commands = register_asset_commands(runtime->commands_, *runtime->assets_, false);
    if (!asset_commands) return std::unexpected(asset_commands.error());
#endif
#ifdef DK_RUNTIME_CAPTURE
    auto capture = CaptureService::create(DK_CAPTURE_SHADER_DIR,[events=runtime->events_] { events->notify(); });
    if (!capture) return std::unexpected(capture.error());
    runtime->captures_=std::move(*capture);
    auto rendering=register_render_commands(runtime->commands_,*runtime->captures_,*runtime->service_);
    if (!rendering) return std::unexpected(rendering.error());
#endif
#if defined(DK_RUNTIME_CAPTURE) || defined(DK_RUNTIME_ASSETS)
    auto jobs = register_job_commands(runtime->commands_,{
        [r=runtime.get()](JobId id) { return r->job(id); },
        [r=runtime.get()](JobId id,std::chrono::milliseconds timeout) { return r->wait_job(id,timeout); },
        [r=runtime.get()](JobId id) { return r->cancel_job(id); }},
#ifdef DK_RUNTIME_CAPTURE
        render_job_result_schema()
#else
        asset_job_result_schema()
#endif
        );
    if (!jobs) return std::unexpected(jobs.error());
#endif
    auto registered = register_scene_commands(runtime->commands_, *runtime->service_);
    if (!registered)
        return std::unexpected(registered.error());
    registered = register_simulation_commands(runtime->commands_, runtime->simulation_, *runtime->service_);
    if (!registered) return std::unexpected(registered.error());
    runtime->tasks_.reserve(257);
    registered = runtime->register_runtime_commands();
    if (!registered)
        return std::unexpected(registered.error());
    return runtime;
}
Result<void> Runtime::register_runtime_commands()
{
    auto r = commands_.add({"runtime.capabilities", "Describe enabled runtime capabilities",
                            schema::object(),
                            schema::object({{"protocol", schema::string()},
                                            {"async_tasks", schema::boolean()},
                                            {"async_jobs", schema::boolean()},
                                            {"render_capture",schema::boolean()},
                                            {"simulation", schema::object({{"fixed_step", schema::boolean()},
                                                {"solver", schema::string()}, {"gpu",schema::boolean()}, {"experiment_export",schema::boolean()},
                                                {"finite_tasks",schema::boolean()},{"max_task_steps",schema::integer()},
                                                {"max_batch_steps",schema::integer()},{"max_in_flight_batches",schema::integer()}},
                                                {"fixed_step", "solver", "gpu", "experiment_export","finite_tasks","max_task_steps","max_batch_steps","max_in_flight_batches"})},
                                            {"capture_limits",schema::nullable(schema::object({{"queued",schema::integer()},
                                                {"active",schema::integer()},{"terminal",schema::integer()},{"input_bytes",schema::integer()},
                                                {"max_dimension",schema::integer()}},{"queued","active","terminal","input_bytes","max_dimension"}))},
                                            {"job_limits", schema::nullable(schema::object({{"queued",schema::integer()},
                                                {"active",schema::integer()},{"terminal",schema::integer()},
                                                {"input_bytes",schema::integer()}},{"queued","active","terminal","input_bytes"}))},
                                            {"task_retention", schema::integer()},
                                            {"max_line_bytes", schema::integer()},
                                            {"max_batch_requests", schema::integer()},
                                            {"transactions", schema::boolean()},
                                            {"guard", schema::string()}},
                                           {"protocol", "async_tasks", "async_jobs", "render_capture", "simulation", "capture_limits", "job_limits", "task_retention", "max_line_bytes",
                                            "max_batch_requests", "transactions", "guard"})},
                           [this](const Json &) -> Result<Json>
                           {
                               Json value{{"protocol", "jsonrpc-2.0-jsonl"}, {"async_tasks", false},
                                           {"task_retention", 256},           {"max_line_bytes", 1024 * 1024},
                                           {"max_batch_requests", 128},       {"transactions", true},
                                           {"guard", "document_id+revision"},
                                           {"simulation", {{"fixed_step", true}, {"solver", "xpbd_cpu"},{"finite_tasks",true},
                                               {"max_task_steps",1000000},{"max_batch_steps",8},{"max_in_flight_batches",1}}}};
                               value["simulation"]["gpu"] = false;
                               value["simulation"]["experiment_export"] = false;
#ifdef DK_SIMULATION_GPU
                               value["simulation"]["gpu"] = true;
                               value["simulation"]["experiment_export"] = true;
#endif
                               value["async_jobs"] = false; value["job_limits"] = nullptr;
#ifdef DK_RUNTIME_ASSETS
                               const auto limits = assets_->limits(); value["async_jobs"] = true;
                               value["job_limits"] = {{"queued",limits.queued},{"active",limits.active},
                                   {"terminal",limits.terminal},{"input_bytes",limits.input_bytes}};
#endif
                               value["render_capture"]=false; value["capture_limits"]=nullptr;
#ifdef DK_RUNTIME_CAPTURE
                               const auto capture_limits=captures_->limits();
                               value["async_jobs"]=true; value["render_capture"]=true;
                               value["capture_limits"]={{"queued",capture_limits.queued},{"active",capture_limits.active},
                                   {"terminal",capture_limits.terminal},{"input_bytes",capture_limits.input_bytes},{"max_dimension",2048}};
#endif
                               return value;
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
                             simulation_.request_shutdown();
                             stopping_ = true;
                             return value;
                         });
}
bool Runtime::has_command(std::string_view method) const
{
    return commands_.describe(method).has_value();
}
void Runtime::pump() {
    if (!stopping_) simulation_.pump();
#ifdef DK_RUNTIME_CAPTURE
    if (!stopping_) captures_->pump();
#endif
#ifdef DK_RUNTIME_ASSETS
    if (!stopping_) assets_->pump();
#endif
}
std::chrono::steady_clock::time_point Runtime::next_pump_deadline(std::chrono::steady_clock::time_point fallback) const {
    return stopping_ ? fallback : simulation_.next_deadline(fallback);
}
Result<CommandExecution> Runtime::dispatch(std::string_view method, const Json &parameters, bool auto_guard)
{
    DK_PROFILE_ZONE("Runtime.Dispatch");
    DK_PROFILE_ZONE_TEXT(method);
    if (stopping_)
        return std::unexpected(Error{ErrorCode::invalid_state, "Runtime is stopping"});
    if (!has_command(method))
        return std::unexpected(Error{ErrorCode::not_found, "Unknown command"});
    if (!parameters.is_object())
        return std::unexpected(Error{ErrorCode::invalid_argument, "Named object parameters required"});
    if (dispatching_)
        return std::unexpected(Error{ErrorCode::invalid_state, "Runtime dispatch is not reentrant"});
    pump();
#ifdef DK_RUNTIME_ASSETS
    auto synchronized = assets_->synchronize_scene(*service_);
    if (!synchronized) return std::unexpected(synchronized.error());
#endif
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
#ifdef DK_RUNTIME_ASSETS
            if (method.starts_with("assets.")) {
                if (auto catalog = assets_->catalog()) effective["guard"] = catalog_guard_json((*catalog)->catalog().guard());
            } else
#endif
            {
            auto state = service_->state();
            if (state)
                effective["guard"] = edit_guard_json({state->document_id, state->revision});
            }
        }
    }
    auto id = TaskId::generate();
    if (!id)
        return std::unexpected(id.error());
    TaskRecord task{*id, std::string(method), false, {}, {}};
#ifdef DK_RUNTIME_ASSETS
    if (method == "project.save" && effective.contains("manifest") && effective["manifest"].is_string()) {
        auto sync = assets_->synchronize_scene(*service_, effective["manifest"].get_ref<const std::string&>());
        if (!sync) return std::unexpected(sync.error());
    }
#endif
    auto result = commands_.execute(method, effective);
#ifdef DK_RUNTIME_ASSETS
    assets_->rethrow_failure();
    if (result && method == "project.save") {
        auto refreshed = assets_->refresh_manifest(effective["manifest"].get<std::string>());
        if (!refreshed) result = std::unexpected(refreshed.error());
    }
    if (result) {
        auto sync = assets_->synchronize_scene(*service_);
        if (!sync) result = std::unexpected(sync.error());
    }
#endif
#ifdef DK_RUNTIME_CAPTURE
    captures_->rethrow_failure();
#endif
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

#if defined(DK_RUNTIME_CAPTURE) || defined(DK_RUNTIME_ASSETS)
namespace dk {
Result<JobSnapshot> Runtime::job(JobId id) const {
#ifdef DK_RUNTIME_CAPTURE
    auto value=captures_->job(id);
#ifdef DK_RUNTIME_ASSETS
    if (!value && value.error().code==ErrorCode::not_found) return assets_->job(id);
#endif
    return value;
#else
    return assets_->job(id);
#endif
}
Result<JobCancel> Runtime::cancel_job(JobId id) {
#ifdef DK_RUNTIME_CAPTURE
    auto value=captures_->cancel(id);
#ifdef DK_RUNTIME_ASSETS
    if (!value && value.error().code==ErrorCode::not_found) return assets_->cancel(id);
#endif
    return value;
#else
    return assets_->cancel(id);
#endif
}
Result<JobWait> Runtime::wait_job(JobId id,std::chrono::milliseconds timeout) {
    if (timeout.count()<0 || timeout.count()>1000) return std::unexpected(Error{ErrorCode::invalid_argument,"timeout_ms must be 0..1000"});
    const auto deadline=JobQueue::Clock::now()+timeout;
    for (;;) {
        const auto sequence=events_->sequence(); pump();
        auto value=job(id); if (!value) return std::unexpected(value.error());
        if (terminal(value->state)) return JobWait{std::move(*value),false};
        if (JobQueue::Clock::now()>=deadline) return JobWait{std::move(*value),true};
        events_->wait(sequence,next_pump_deadline(deadline));
    }
}
}
#endif

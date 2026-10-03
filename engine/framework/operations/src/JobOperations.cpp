#include <dk/operations/JobOperations.hpp>
namespace dk {
namespace {
Json uuid() { return schema::string(36,36); }
Json integer(std::uint64_t max = UINT64_MAX) { auto v = schema::integer(); v["minimum"] = 0; v["maximum"] = max; return v; }
Json error_schema() { return schema::nullable(schema::object({{"code",integer()},{"message",schema::string(0,4096)},
    {"context",schema::array(schema::string(0,512),0,8)}},{"code","message","context"})); }
Json error_json(const std::optional<Error>& error) { return error ? Json{{"code",static_cast<unsigned>(error->code)},
    {"message",error->message},{"context",error->context}} : Json(nullptr); }
Json job_schema(const Json& result_schema) { return schema::object({{"id",uuid()},{"state",{{"type","string"},{"enum",{"queued","running","succeeded","failed","cancelled"}}}},
    {"cancel_requested",schema::boolean()},{"result",schema::nullable(result_schema)},{"error",error_schema()}},
    {"id","state","cancel_requested","result","error"}); }
Json job_json(const JobSnapshot& job) {
    constexpr const char* states[] = {"queued","running","succeeded","failed","cancelled"};
    return {{"id",job.id.to_string()},{"state",states[static_cast<unsigned>(job.state)]},{"cancel_requested",job.cancel_requested},
        {"result",job.summary.empty() ? Json(nullptr) : Json::parse(job.summary)},{"error",error_json(job.error)}};
}
template<class Id> Result<Id> id(const Json& value) {
    auto result=Id::parse(value.get<std::string>()); if (!result) return result;
    if (result->is_nil()) return std::unexpected(Error{ErrorCode::invalid_argument,"UUID must not be nil"}); return result;
}
}
Result<void> register_job_commands(CommandRegistry& registry, JobCommands backend, Json result_schema) {
    auto add = [&](std::string name, std::string description, Json params, Json result, CommandEffect effect, CommandHandler handler) {
        return registry.add({std::move(name),std::move(description),std::move(params),std::move(result),effect,false},std::move(handler));
    };
    auto r = add("jobs.get","Query a retained background JobId",schema::object({{"id",uuid()}},{"id"}),job_schema(result_schema),CommandEffect::query,
        [backend](const Json& p) -> Result<Json> {
            auto job = id<JobId>(p["id"]); if (!job) return std::unexpected(job.error()); auto result = backend.get(*job);
            if (!result) return std::unexpected(result.error()); return job_json(*result);
        }); if (!r) return r;
    r = add("jobs.wait","Pump completions and wait at most 1000 ms without command reentry",
        schema::object({{"id",uuid()},{"timeout_ms",integer(1000)}},{"id"}),
        schema::object({{"job",job_schema(result_schema)},{"timed_out",schema::boolean()}},{"job","timed_out"}),CommandEffect::query,
        [backend](const Json& p) -> Result<Json> {
            auto job = id<JobId>(p["id"]); if (!job) return std::unexpected(job.error());
            auto result = backend.wait(*job,std::chrono::milliseconds{p.value("timeout_ms",0)});
            if (!result) return std::unexpected(result.error()); return Json{{"job",job_json(result->job)},{"timed_out",result->timed_out}};
        }); if (!r) return r;
    return add("jobs.cancel","Request cooperative cancellation before publication",schema::object({{"id",uuid()}},{"id"}),
        schema::object({{"job",job_schema(result_schema)},{"accepted",schema::boolean()}},{"job","accepted"}),CommandEffect::control,
        [backend](const Json& p) -> Result<Json> {
            auto job = id<JobId>(p["id"]); if (!job) return std::unexpected(job.error()); auto result = backend.cancel(*job);
            if (!result) return std::unexpected(result.error()); return Json{{"job",job_json(result->job)},{"accepted",result->accepted}};
        });
}
}

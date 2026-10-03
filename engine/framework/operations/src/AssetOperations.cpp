#include <dk/operations/AssetOperations.hpp>
#include <algorithm>
#include <dk/operations/JobOperations.hpp>

namespace dk {
namespace {
Json uuid() { return schema::string(36,36); }
Json integer(std::uint64_t max = UINT64_MAX) { auto v = schema::integer(); v["minimum"] = 0; v["maximum"] = max; return v; }
Json guard_schema() { return schema::object({{"session_id",uuid()},{"revision",integer()}},{"session_id","revision"}); }
Json catalog_state_schema() { return schema::object({{"guard",guard_schema()},{"total",integer(10000)}},{"guard","total"}); }
Json error_schema() { return schema::nullable(schema::object({{"code",integer()},{"message",schema::string(0,4096)},
    {"context",schema::array(schema::string(0,512),0,8)}},{"code","message","context"})); }
Json error_json(const std::optional<Error>& error) { return error ? Json{{"code",static_cast<unsigned>(error->code)},
    {"message",error->message},{"context",error->context}} : Json(nullptr); }
Json result_schema() { return schema::object({{"root_id",uuid()},{"key",schema::string(32,32)},{"cache_hit",schema::boolean()}},{"root_id","key","cache_hit"}); }
Json kind_schema() { return {{"type","string"},{"enum",{"mesh","material","texture"}}}; }
Json asset_schema() { return schema::object({{"id",uuid()},{"kind",kind_schema()},
    {"state",{{"type","string"},{"enum",{"unloaded","loading","ready","failed"}}}}, {"generation",integer()},
    {"ready_generation",integer()},{"job_id",schema::nullable(uuid())},{"artifact",schema::nullable(result_schema())},{"error",error_schema()}},
    {"id","kind","state","generation","ready_generation","job_id","artifact","error"}); }
Json asset_json(const AssetStatus& state, const AsyncAssetService& service) {
    constexpr const char* states[] = {"unloaded","loading","ready","failed"};
    AssetKind kind = AssetKind::mesh;
    if (auto catalog = service.catalog()) for (const auto& record : (*catalog)->catalog().records()) if (record.id == state.id) kind = record.kind;
    Json artifact = nullptr;
    if (state.data) artifact = {{"root_id",state.data->artifact.data.mesh.id.to_string()},
        {"key",std::string{state.data->key}},{"cache_hit",state.data->cache_hit}};
    return {{"id",state.id.to_string()},{"kind",asset_kind_name(kind)},{"state",states[static_cast<unsigned>(state.state)]},
        {"generation",state.generation},{"ready_generation",state.ready_generation},
        {"job_id",state.job ? Json(state.job->to_string()) : Json(nullptr)},{"artifact",std::move(artifact)},{"error",error_json(state.error)}};
}
template<class Id> Result<Id> id(const Json& value) {
    auto result = Id::parse(value.get<std::string>()); if (!result) return result;
    if (result->is_nil()) return std::unexpected(Error{ErrorCode::invalid_argument,"UUID must not be nil"}); return result;
}
Result<CatalogGuard> guard(const Json& value) {
    auto session = id<CatalogSessionId>(value["session_id"]); if (!session) return std::unexpected(session.error());
    return CatalogGuard{*session,value["revision"].get<std::uint64_t>()};
}
Result<Json> catalog_state(const AsyncAssetService& service) {
    auto c = service.catalog(); if (!c) return std::unexpected(c.error());
    return Json{{"guard",catalog_guard_json((*c)->catalog().guard())},{"total",(*c)->catalog().records().size()}};
}
}
Json catalog_guard_json(CatalogGuard guard) { return {{"session_id",guard.session_id.to_string()},{"revision",guard.revision}}; }
Result<void> register_asset_commands(CommandRegistry& registry, AsyncAssetService& service, bool with_jobs) {
    auto add = [&](std::string name, std::string description, Json params, Json result, CommandEffect effect, CommandHandler handler) {
        return registry.add({std::move(name),std::move(description),std::move(params),std::move(result),effect,false},std::move(handler));
    };
    auto r = add("assets.open","Open a project asset catalog without loading a scene",
        schema::object({{"manifest",schema::string(1,4096)},{"guard",guard_schema()}},{"manifest"}),catalog_state_schema(),CommandEffect::control,
        [&service](const Json& p) -> Result<Json> {
            std::optional<CatalogGuard> g;
            if (p.contains("guard")) { auto v = guard(p["guard"]); if (!v) return std::unexpected(v.error()); g = *v; }
            auto opened = service.open(p["manifest"].get<std::string>(),g); if (!opened) return std::unexpected(opened.error());
            return catalog_state(service);
        }); if (!r) return r;
    const auto record = schema::object({{"id",uuid()},{"kind",kind_schema()},{"path",schema::string(1,4096)}},{"id","kind","path"});
    r = add("assets.catalog","Query catalog guard and paginated registered identities",
        schema::object({{"offset",integer(10000)},{"limit",[] { auto s=integer(256); s["minimum"]=1; return s; }()}}),
        schema::object({{"guard",guard_schema()},{"total",integer(10000)},{"offset",integer(10000)},
            {"has_more",schema::boolean()},{"records",schema::array(record,0,256)}},{"guard","total","offset","has_more","records"}),CommandEffect::query,
        [&service](const Json& p) -> Result<Json> {
            auto c = service.catalog(); if (!c) return std::unexpected(c.error()); const auto records = (*c)->catalog().records();
            const auto offset = p.value("offset",std::size_t{0}), limit = p.value("limit",std::size_t{128});
            auto values = Json::array(); const auto end = std::min(records.size(),offset+limit);
            for (auto i = offset; i < end; ++i) values.push_back({{"id",records[i].id.to_string()},{"kind",asset_kind_name(records[i].kind)},{"path",std::string{records[i].path}}});
            return Json{{"guard",catalog_guard_json((*c)->catalog().guard())},{"total",records.size()},
                {"offset",offset},{"has_more",end < records.size()},{"records",std::move(values)}};
        }); if (!r) return r;
    r = add("assets.import","Submit CPU import/cache preparation; completion publishes metadata and current",
        schema::object({{"source",schema::string(1,4096)},{"unit_scale",schema::number()}},{"source"}),
        schema::object({{"job_id",uuid()}},{"job_id"}),CommandEffect::external,
        [&service](const Json& p) -> Result<Json> {
            auto job = service.import(p["source"].get<std::string>(),p.contains("unit_scale") ? std::optional{p["unit_scale"].get<double>()} : std::nullopt);
            if (!job) return std::unexpected(job.error()); return Json{{"job_id",job->to_string()}};
        }); if (!r) return r;
    r = add("assets.register","Persist source output identities into the active catalog",
        schema::object({{"guard",guard_schema()},{"source",schema::string(1,4096)},
            {"create_meta",schema::boolean()},{"unit_scale",schema::number()},
            {"outputs",schema::array(schema::object({{"key",schema::string(1,13)},{"kind",kind_schema()}},{"key","kind"}),0,10000)}},{"guard","source"}),
        catalog_state_schema(),CommandEffect::external,[&service](const Json& p) -> Result<Json> {
            auto g = guard(p["guard"]); if (!g) return std::unexpected(g.error());
            std::vector<AssetOutputSpec> outputs;
            if (p.contains("outputs")) for (const auto& output : p["outputs"]) {
                auto kind = parse_asset_kind(output["kind"].get<std::string>()); if (!kind) return std::unexpected(kind.error());
                outputs.push_back({output["key"].get_ref<const std::string&>(),*kind});
            }
            auto result = service.register_source(*g,{p["source"].get_ref<const std::string&>(),outputs,
                p.contains("unit_scale") ? std::optional{p["unit_scale"].get<double>()} : std::nullopt,
                p.value("create_meta",false) ? MissingMetaPolicy::create_or_adopt : MissingMetaPolicy::reject});
            if (!result) return std::unexpected(result.error()); return catalog_state(service);
        }); if (!r) return r;
    r = add("assets.rename","Rename a registered source and its sidecar within the same directory",
        schema::object({{"guard",guard_schema()},{"source",schema::string(1,4096)},{"target",schema::string(1,4096)}},{"guard","source","target"}),
        catalog_state_schema(),CommandEffect::external,[&service](const Json& p) -> Result<Json> {
            auto g = guard(p["guard"]); if (!g) return std::unexpected(g.error());
            auto result = service.rename_source(*g,p["source"].get<std::string>(),p["target"].get<std::string>());
            if (!result) return std::unexpected(result.error()); return catalog_state(service);
        }); if (!r) return r;
    for (const auto method : {"assets.load","assets.status","assets.unload"}) {
        r = add(method,"Get, load or release the current CPU asset generation",schema::object({{"id",uuid()}},{"id"}),asset_schema(),
            std::string_view{method} == "assets.status" ? CommandEffect::query : CommandEffect::control,
            [&service,method](const Json& p) -> Result<Json> {
                auto asset = id<AssetId>(p["id"]); if (!asset) return std::unexpected(asset.error());
                auto result = std::string_view{method} == "assets.load" ? service.load(*asset) :
                    std::string_view{method} == "assets.unload" ? service.unload(*asset) : service.status(*asset);
                if (!result) return std::unexpected(result.error()); return asset_json(*result,service);
            }); if (!r) return r;
    }
    if (!with_jobs) return {};
    return register_job_commands(registry,{
        [&service](JobId id) { return service.job(id); },
        [&service](JobId id,std::chrono::milliseconds timeout) { return service.wait(id,timeout); },
        [&service](JobId id) { return service.cancel(id); }},result_schema());
}
}

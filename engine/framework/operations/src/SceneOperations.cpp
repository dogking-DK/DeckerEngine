#include <dk/operations/SceneOperations.hpp>
#include <limits>

namespace dk
{
namespace
{
Json id_schema()
{
    return schema::string(36, 36);
}
Json revision_schema()
{
    auto s = schema::integer();
    s["minimum"] = 0;
    s["maximum"] = std::numeric_limits<std::uint64_t>::max();
    return s;
}
Json guard_schema()
{
    return schema::object({{"document_id", id_schema()}, {"revision", revision_schema()}},
                          {"document_id", "revision"});
}
Json state_schema()
{
    return schema::object({{"document_id", id_schema()},
                           {"scene_id", id_schema()},
                           {"revision", revision_schema()},
                           {"dirty", schema::boolean()},
                           {"entity_count", revision_schema()}},
                          {"document_id", "scene_id", "revision", "dirty", "entity_count"});
}
Json transform_schema()
{
    return schema::object({{"translation", schema::array(schema::number(), 3, 3)},
                           {"rotation", schema::array(schema::number(), 4, 4)},
                           {"scale", schema::array(schema::number(), 3, 3)}},
                          {"translation", "rotation", "scale"});
}
Json asset_schema(bool record = false)
{
    auto properties =
        Json{{"id", id_schema()}, {"kind", {{"type", "string"}, {"enum", {"mesh", "material", "texture"}}}}};
    auto required = Json::array({"id", "kind"});
    if (record)
    {
        properties["path"] = schema::string(1, 4096);
        required.push_back("path");
    }
    return schema::object(properties, required);
}
Json entity_schema()
{
    return schema::object({{"id", id_schema()},
                           {"name", schema::string()},
                           {"transform", transform_schema()},
                           {"parent", schema::nullable(id_schema())},
                           {"assets", schema::array(asset_schema(), 0, 64)},
                           {"world_matrix", schema::array(schema::number(), 16, 16)}},
                          {"id", "name", "transform", "parent", "assets", "world_matrix"});
}
template <typename Id> Result<Id> read_id(const Json &value)
{
    if (!value.is_string())
        return std::unexpected(Error{ErrorCode::invalid_argument, "Expected UUID string"});
    auto id = Id::parse(value.get_ref<const std::string &>());
    if (!id)
        return id;
    if (id->is_nil())
        return std::unexpected(Error{ErrorCode::invalid_argument, "UUID must not be nil"});
    return id;
}
Result<Json> state_result(const SceneService &service, const Result<void> &result)
{
    if (!result)
        return std::unexpected(result.error());
    auto state = service.state();
    if (!state)
        return std::unexpected(state.error());
    return document_state_json(*state);
}
Result<std::optional<EditGuard>> optional_guard(const Json &p)
{
    if (!p.contains("guard"))
        return std::optional<EditGuard>{};
    auto g = parse_edit_guard(p["guard"]);
    if (!g)
        return std::unexpected(g.error());
    return std::optional<EditGuard>(*g);
}
Result<Json> entity_json(const SceneDocument &doc, EntityId id)
{
    auto data = doc.entity(id);
    if (!data)
        return std::unexpected(data.error());
    auto world = doc.world_transform(id);
    if (!world)
        return std::unexpected(world.error());
    Json assets = Json::array();
    for (const auto &a : data->assets)
        assets.push_back({{"id", a.id.to_string()}, {"kind", asset_kind_name(a.kind)}});
    Json matrix = Json::array();
    for (int row = 0; row < 4; ++row)
        for (int col = 0; col < 4; ++col)
            matrix.push_back(world->matrix()(row, col));
    const auto &t = data->local;
    return Json{{"id", id.to_string()},
                {"name", data->name},
                {"transform",
                 {{"translation", {t.translation.x(), t.translation.y(), t.translation.z()}},
                  {"rotation", {t.rotation.x(), t.rotation.y(), t.rotation.z(), t.rotation.w()}},
                  {"scale", {t.scale.x(), t.scale.y(), t.scale.z()}}}},
                {"parent", data->parent ? Json(data->parent->to_string()) : Json(nullptr)},
                {"assets", assets},
                {"world_matrix", matrix}};
}
} // namespace
Json document_state_json(DocumentState s)
{
    return {{"document_id", s.document_id.to_string()},
            {"scene_id", s.scene_id.to_string()},
            {"revision", s.revision},
            {"dirty", s.dirty},
            {"entity_count", s.entity_count}};
}
Json document_state_schema() { return state_schema(); }
Json edit_guard_json(EditGuard g)
{
    return {{"document_id", g.document_id.to_string()}, {"revision", g.revision}};
}
Result<EditGuard> parse_edit_guard(const Json &value)
{
    auto valid = schema::validate(guard_schema(), value);
    if (!valid)
        return std::unexpected(valid.error());
    auto id = read_id<DocumentId>(value["document_id"]);
    if (!id)
        return std::unexpected(id.error());
    return EditGuard{*id, value["revision"].get<std::uint64_t>()};
}
Result<SceneEdit> decode_scene_edit(std::string_view method, const Json &p)
{
    if (method == "entity.create")
    {
        std::optional<EntityId> id;
        if (p.contains("id"))
        {
            auto parsed = read_id<EntityId>(p["id"]);
            if (!parsed)
                return std::unexpected(parsed.error());
            id = *parsed;
        }
        return CreateEntity{id};
    }
    auto id = read_id<EntityId>(p.at("id"));
    if (!id)
        return std::unexpected(id.error());
    if (method == "entity.delete")
        return DeleteEntity{*id};
    if (method == "entity.set_name")
        return SetName{*id, p.at("name").get<std::string>()};
    if (method == "entity.set_parent")
    {
        std::optional<EntityId> parent;
        if (!p.at("parent").is_null())
        {
            auto parsed = read_id<EntityId>(p["parent"]);
            if (!parsed)
                return std::unexpected(parsed.error());
            parent = *parsed;
        }
        return SetParent{*id, parent};
    }
    if (method == "entity.set_transform")
    {
        Trsd t;
        const auto &v = p.at("transform");
        for (int i = 0; i < 3; ++i)
        {
            t.translation[i] = v["translation"][i].get<double>();
            t.scale[i] = v["scale"][i].get<double>();
        }
        t.rotation = Quatd(v["rotation"][3].get<double>(), v["rotation"][0].get<double>(),
                           v["rotation"][1].get<double>(), v["rotation"][2].get<double>());
        return SetTransform{*id, t};
    }
    if (method == "entity.set_assets")
    {
        std::vector<AssetReference> assets;
        for (const auto &a : p.at("assets"))
        {
            auto asset = read_id<AssetId>(a["id"]);
            if (!asset)
                return std::unexpected(asset.error());
            auto kind = parse_asset_kind(a["kind"].get<std::string>());
            if (!kind)
                return std::unexpected(kind.error());
            assets.push_back({*asset, *kind});
        }
        return SetAssets{*id, std::move(assets)};
    }
    return std::unexpected(Error{ErrorCode::invalid_argument, "Not a scene edit: " + std::string(method)});
}
Result<void> register_scene_commands(CommandRegistry &registry, SceneService &service)
{
    auto add = [&](std::string name, std::string description, Json parameters, Json result,
                   CommandEffect effect, CommandHandler handler)
    {
        return registry.add({std::move(name), std::move(description), std::move(parameters),
                             std::move(result), effect, effect == CommandEffect::memory_edit},
                            std::move(handler));
    };
    auto r = add("scene.new", "Create a new scene session",
                 schema::object({{"guard", guard_schema()},
                                 {"name", schema::string(1)},
                                 {"scene_file", schema::string(1, 4096)},
                                 {"assets", schema::array(asset_schema(true))}}),
                 state_schema(), CommandEffect::control,
                 [&service](const Json &p) -> Result<Json>
                 {
                     auto g = optional_guard(p);
                     if (!g)
                         return std::unexpected(g.error());
                     auto description = parse_project(Json{{"format", "DeckerProject"},
                                                           {"version", 1},
                                                           {"name", p.value("name", "Untitled")},
                                                           {"scene", p.value("scene_file", "scene.json")},
                                                           {"assets", p.value("assets", Json::array())}}
                                                          .dump());
                     if (!description)
                         return std::unexpected(description.error());
                     return state_result(service, service.new_scene(std::move(*description), *g));
                 });
    if (!r)
        return r;
    r = add("scene.load", "Load a project into a new scene session",
            schema::object({{"guard", guard_schema()}, {"manifest", schema::string(1, 4096)}}, {"manifest"}),
            state_schema(), CommandEffect::control,
            [&service](const Json &p) -> Result<Json>
            {
                auto g = optional_guard(p);
                if (!g)
                    return std::unexpected(g.error());
                auto path = path_from_utf8(p["manifest"].get<std::string>());
                if (!path)
                    return std::unexpected(path.error());
                return state_result(service, service.load(*path, *g));
            });
    if (!r)
        return r;
    r = add("scene.save", "Save the current scene atomically",
            schema::object({{"guard", guard_schema()}}, {"guard"}), state_schema(), CommandEffect::external,
            [&service](const Json &p) -> Result<Json>
            {
                auto g = parse_edit_guard(p["guard"]);
                if (!g)
                    return std::unexpected(g.error());
                return state_result(service, service.save(*g));
            });
    if (!r)
        return r;
    r = add("project.save", "Save the project manifest separately from the scene",
            schema::object({{"guard", guard_schema()}, {"manifest", schema::string(1, 4096)}},
                           {"guard", "manifest"}),
            state_schema(), CommandEffect::external,
            [&service](const Json &p) -> Result<Json>
            {
                auto g = parse_edit_guard(p["guard"]);
                if (!g)
                    return std::unexpected(g.error());
                auto path = path_from_utf8(p["manifest"].get<std::string>());
                if (!path)
                    return std::unexpected(path.error());
                return state_result(service, service.save_manifest(*g, *path));
            });
    if (!r)
        return r;
    auto offset_schema = revision_schema();
    offset_schema["maximum"] = 10000;
    auto limit_schema = revision_schema();
    limit_schema["minimum"] = 1;
    limit_schema["maximum"] = 256;
    r = add("scene.query", "Query a page of entities ordered by persistent ID",
            schema::object({{"offset", offset_schema}, {"limit", limit_schema}}),
            schema::object({{"state", state_schema()},
                            {"entities", schema::array(entity_schema(), 0, 256)},
                            {"offset", offset_schema},
                            {"has_more", schema::boolean()}},
                           {"state", "entities", "offset", "has_more"}),
            CommandEffect::query,
            [&service](const Json &p) -> Result<Json>
            {
                auto doc = service.document();
                if (!doc)
                    return std::unexpected(doc.error());
                Json entities = Json::array();
                const auto ids = (*doc)->entity_ids();
                const auto offset = p.value("offset", std::size_t{0});
                const auto end = std::min(ids.size(), offset + p.value("limit", std::size_t{128}));
                for (auto i = offset; i < end; ++i)
                {
                    auto entity = entity_json(**doc, ids[i]);
                    if (!entity)
                        return entity;
                    entities.push_back(std::move(*entity));
                }
                return Json{{"state", document_state_json(*service.state())},
                            {"entities", std::move(entities)},
                            {"offset", offset},
                            {"has_more", end < ids.size()}};
            });
    if (!r)
        return r;
    r = add("entity.get", "Query one entity and its world matrix",
            schema::object({{"id", id_schema()}}, {"id"}), entity_schema(), CommandEffect::query,
            [&service](const Json &p) -> Result<Json>
            {
                auto doc = service.document();
                if (!doc)
                    return std::unexpected(doc.error());
                auto id = read_id<EntityId>(p["id"]);
                if (!id)
                    return std::unexpected(id.error());
                return entity_json(**doc, *id);
            });
    if (!r)
        return r;
    for (const auto *method : {"entity.create", "entity.delete", "entity.set_name", "entity.set_transform",
                               "entity.set_parent", "entity.set_assets"})
    {
        const std::string name(method);
        Json properties{{"guard", guard_schema()}, {"id", id_schema()}};
        Json required = {"guard"};
        if (name != "entity.create")
            required.push_back("id");
        if (name == "entity.set_name")
        {
            properties["name"] = schema::string();
            required.push_back("name");
        }
        if (name == "entity.set_transform")
        {
            properties["transform"] = transform_schema();
            required.push_back("transform");
        }
        if (name == "entity.set_parent")
        {
            properties["parent"] = schema::nullable(id_schema());
            required.push_back("parent");
        }
        if (name == "entity.set_assets")
        {
            properties["assets"] = schema::array(asset_schema(), 0, 64);
            required.push_back("assets");
        }
        r = add(name, "Edit scene: " + name, schema::object(properties, required),
                schema::object({{"state", state_schema()}, {"created_id", schema::nullable(id_schema())}},
                               {"state", "created_id"}),
                CommandEffect::memory_edit,
                [&service, name](const Json &p) -> Result<Json>
                {
                    auto guard = parse_edit_guard(p["guard"]);
                    if (!guard)
                        return std::unexpected(guard.error());
                    auto edit = decode_scene_edit(name, p);
                    if (!edit)
                        return std::unexpected(edit.error());
                    auto result = service.edit(*guard, *edit);
                    if (!result)
                        return std::unexpected(result.error());
                    return Json{{"state", document_state_json(*service.state())},
                                {"created_id", *result ? Json((*result)->to_string()) : Json(nullptr)}};
                });
        if (!r)
            return r;
    }
    r = add(
        "scene.transaction", "Commit an atomic batch of undoable entity edits",
        schema::object({{"guard", guard_schema()},
                        {"commands", schema::array(schema::object({{"method", schema::string(1, 96)},
                                                                   {"params", {{"type", "object"}}}},
                                                                  {"method", "params"}),
                                                   1, 128)}},
                       {"guard", "commands"}),
        schema::object({{"state", state_schema()},
                        {"created_ids", schema::array(schema::nullable(id_schema()), 1, 128)}},
                       {"state", "created_ids"}),
        CommandEffect::memory_edit,
        [&service, &registry](const Json &p) -> Result<Json>
        {
            auto guard = parse_edit_guard(p["guard"]);
            if (!guard)
                return std::unexpected(guard.error());
            std::vector<SceneEdit> edits;
            for (const auto &item : p["commands"])
            {
                const auto name = item["method"].get<std::string>();
                if (name != "entity.create" && name != "entity.delete" && name != "entity.set_name" &&
                    name != "entity.set_transform" && name != "entity.set_parent" &&
                    name != "entity.set_assets")
                    return std::unexpected(Error{
                        ErrorCode::invalid_argument, "Command is not permitted in a transaction", {name}});
                auto params = item["params"];
                if (params.contains("guard"))
                    return std::unexpected(Error{ErrorCode::invalid_argument,
                                                 "Transaction items must not supply their own guard"});
                params["guard"] = p["guard"];
                auto valid = registry.validate_parameters(name, params);
                if (!valid)
                    return std::unexpected(valid.error().with_context(name));
                auto edit = decode_scene_edit(name, params);
                if (!edit)
                    return std::unexpected(edit.error().with_context(name));
                edits.push_back(std::move(*edit));
            }
            auto result = service.edit_batch(*guard, edits);
            if (!result)
                return std::unexpected(result.error());
            Json ids = Json::array();
            for (const auto &id : *result)
                ids.push_back(id ? Json(id->to_string()) : Json(nullptr));
            return Json{{"state", document_state_json(*service.state())}, {"created_ids", std::move(ids)}};
        });
    if (!r)
        return r;
    r = add("history.status", "Query bounded undo and redo history", schema::object(),
            schema::object({{"undo_count", revision_schema()},
                            {"redo_count", revision_schema()},
                            {"logical_bytes", revision_schema()}},
                           {"undo_count", "redo_count", "logical_bytes"}),
            CommandEffect::query,
            [&service](const Json &) -> Result<Json>
            {
                const auto h = service.history_status();
                return Json{{"undo_count", h.undo_count},
                            {"redo_count", h.redo_count},
                            {"logical_bytes", h.logical_bytes}};
            });
    if (!r)
        return r;
    for (const bool redo : {false, true})
    {
        r = registry.add({redo ? "history.redo" : "history.undo",
                          redo ? "Redo the next memory edit" : "Undo the last memory edit",
                          schema::object({{"guard", guard_schema()}}, {"guard"}), state_schema(),
                          CommandEffect::memory_edit, false},
                         [&service, redo](const Json &p) -> Result<Json>
                         {
                             auto guard = parse_edit_guard(p["guard"]);
                             if (!guard)
                                 return std::unexpected(guard.error());
                             return state_result(service, redo ? service.redo(*guard) : service.undo(*guard));
                         });
        if (!r)
            return r;
    }
    return {};
}
} // namespace dk

#include <dk/scene/SceneIO.hpp>
#include <dk/io/File.hpp>
#include "JsonSupport.hpp"
#include "ScenePersistence.hpp"
#include <cmath>

namespace dk {
namespace {
using namespace detail;
Json vec3(const Vec3d& value) { return Json::array({value.x(), value.y(), value.z()}); }
double number(const Json& value)
{
    require(value.is_number(), "Expected a finite numeric transform component");
    const auto result = value.get<double>();
    require(std::isfinite(result), "Nonfinite transform component");
    return result;
}
Vec3d read_vec3(const Json& value)
{
    require(value.is_array() && value.size() == 3, "Expected a three-element vector");
    return {number(value[0]), number(value[1]), number(value[2])};
}
Json entity_json(const EntityData& data)
{
    auto references = Json::array();
    for (const auto& reference : data.assets) {
        references.push_back({{"id", reference.id.to_string()}, {"kind", asset_kind_name(reference.kind)}});
    }
    const auto& q = data.local.rotation;
    Json components{
        {"dk.Identity", {{"version", 1}, {"id", data.id.to_string()}}},
        {"dk.Name", {{"version", 1}, {"value", data.name}}},
        {"dk.Transform", {{"version", 1}, {"translation", vec3(data.local.translation)},
            {"rotation", Json::array({q.x(), q.y(), q.z(), q.w()})}, {"scale", vec3(data.local.scale)}}},
        {"dk.Hierarchy", {{"version", 1}, {"parent", data.parent ? Json(data.parent->to_string()) : Json(nullptr)}}},
        {"dk.AssetReferences", {{"version", 1}, {"items", std::move(references)}}}};
    return Json{{"components", std::move(components)}};
}
EntityData read_entity(const Json& value)
{
    fields(value, {"components"});
    const auto& components = value.at("components");
    fields(components, {"dk.Identity", "dk.Name", "dk.Transform", "dk.Hierarchy", "dk.AssetReferences"});
    for (const auto& descriptor : scene_component_descriptors()) {
        const auto& component = components.at(descriptor.name);
        require(component.is_object() && component.contains("version"), "Component version is missing");
        version_one(component.at("version"));
    }
    const auto& identity = components.at("dk.Identity");
    fields(identity, {"version", "id"});
    EntityData data;
    data.id = read_id<EntityId>(identity.at("id"));
    const auto& name = components.at("dk.Name");
    fields(name, {"version", "value"});
    data.name = string_value(name.at("value"));
    const auto& transform = components.at("dk.Transform");
    fields(transform, {"version", "translation", "rotation", "scale"});
    data.local.translation = read_vec3(transform.at("translation"));
    data.local.scale = read_vec3(transform.at("scale"));
    const auto& rotation = transform.at("rotation");
    require(rotation.is_array() && rotation.size() == 4, "Expected quaternion [x,y,z,w]");
    data.local.rotation = Quatd{number(rotation[3]), number(rotation[0]), number(rotation[1]), number(rotation[2])};
    const auto& hierarchy = components.at("dk.Hierarchy");
    fields(hierarchy, {"version", "parent"});
    if (!hierarchy.at("parent").is_null()) { data.parent = read_id<EntityId>(hierarchy.at("parent")); }
    const auto& assets = components.at("dk.AssetReferences");
    fields(assets, {"version", "items"});
    require(assets.at("items").is_array() && assets.at("items").size() <= 64, "Invalid asset reference array");
    for (const auto& item : assets.at("items")) {
        fields(item, {"id", "kind"});
        const auto kind = parse_asset_kind(string_value(item.at("kind")));
        require(kind.has_value(), "Unknown asset kind");
        data.assets.push_back({read_id<AssetId>(item.at("id")), *kind});
    }
    return data;
}
Result<std::string> read_text(const std::filesystem::path& path)
{
    const auto bytes = read_file_bytes(path, json_limit);
    if (!bytes) { return std::unexpected(bytes.error()); }
    if (bytes->empty()) { return std::string{}; }
    return std::string(reinterpret_cast<const char*>(bytes->data()), bytes->size());
}
Result<void> check_references(const SceneSnapshot& snapshot, const Project& project)
{
    for (const auto& entity : snapshot.entities()) {
        for (const auto& reference : entity.assets) {
            const auto path = project.resolve_asset(reference);
            if (!path) { return std::unexpected(path.error().with_context(entity.id.to_string())); }
        }
    }
    return {};
}
Result<void> verify_text(const std::filesystem::path& path, std::string_view expected)
{
    const auto actual = read_text(path);
    if (!actual) { return std::unexpected(actual.error()); }
    if (*actual != expected) {
        return std::unexpected(Error{ErrorCode::io_error, "Temporary file readback differs from snapshot", {"save.verify"}});
    }
    return {};
}
} // namespace

Result<std::string> serialize_scene(const SceneSnapshot& snapshot, const Project& project)
{
    const auto references = check_references(snapshot, project);
    if (!references) { return std::unexpected(references.error()); }
    try {
        auto entities = detail::Json::array();
        for (const auto& entity : snapshot.entities()) { entities.push_back(entity_json(entity)); }
        detail::Json json{{"format", "DeckerScene"}, {"version", 1}, {"scene_id", snapshot.id().to_string()},
            {"revision", snapshot.revision()}, {"entities", std::move(entities)}};
        auto text = json.dump(2) + "\n";
        detail::require(text.size() <= detail::json_limit, "Scene exceeds JSON size limit");
        return text;
    } catch (const detail::FormatError& error) { return std::unexpected(detail::format_error(error, "serialize_scene")); }
      catch (const detail::Json::exception& error) { return std::unexpected(detail::json_error(error, "serialize_scene")); }
}

Result<std::unique_ptr<SceneDocument>> parse_scene(std::string_view text, const Project& project)
{
    try {
        const auto json = detail::parse_json(text);
        detail::fields(json, {"format", "version", "scene_id", "revision", "entities"});
        detail::require(json.at("format") == "DeckerScene", "Unknown scene format");
        detail::version_one(json.at("version"));
        const auto id = detail::read_id<SceneId>(json.at("scene_id"));
        const auto& revision = json.at("revision");
        detail::require(revision.is_number_unsigned() || (revision.is_number_integer() && revision >= 0),
            "Revision must be a uint64 integer");
        const auto& values = json.at("entities");
        detail::require(values.is_array() && values.size() <= 10000, "Invalid entity array or entity limit exceeded");
        std::vector<EntityData> entities;
        entities.reserve(values.size());
        for (const auto& value : values) { entities.push_back(read_entity(value)); }
        auto document = detail::ScenePersistence::build(id, revision.get<std::uint64_t>(), std::move(entities));
        if (!document) { return document; }
        const auto references = check_asset_references(**document, project);
        if (!references) { return std::unexpected(references.error()); }
        return document;
    } catch (const detail::FormatError& error) { return std::unexpected(detail::format_error(error, "parse_scene")); }
      catch (const detail::Json::exception& error) { return std::unexpected(detail::json_error(error, "parse_scene")); }
}

Result<std::unique_ptr<SceneDocument>> load_scene(const Project& project)
{
    const auto path = project.scene_path();
    if (!path) { return std::unexpected(path.error()); }
    const auto text = read_text(*path);
    if (!text) { return std::unexpected(text.error()); }
    auto document = parse_scene(*text, project);
    if (!document) { return document; }
    detail::ScenePersistence::loaded(**document);
    return document;
}

Result<void> reload_scene(std::unique_ptr<SceneDocument>& document, const Project& project)
{
    auto candidate = load_scene(project);
    if (!candidate) { return std::unexpected(candidate.error()); }
    document.swap(*candidate);
    return {};
}

Result<void> save_scene(SceneDocument& document, const SceneSnapshot& snapshot, const Project& project)
{
    if (!detail::ScenePersistence::owns(document, snapshot)) {
        return std::unexpected(Error{ErrorCode::invalid_argument, "Snapshot belongs to another document instance", {"save_scene"}});
    }
    const auto text = serialize_scene(snapshot, project);
    if (!text) { return std::unexpected(text.error()); }
    const auto path = project.scene_path();
    if (!path) { return std::unexpected(path.error()); }
    const auto saved = write_file_bytes_atomic(*path, std::as_bytes(std::span{text->data(), text->size()}),
        [&](const std::filesystem::path& temporary) -> Result<void> {
            const auto verified = verify_text(temporary, *text);
            if (!verified) { return verified; }
            const auto parsed = parse_scene(*text, project);
            if (!parsed) { return std::unexpected(parsed.error()); }
            return {};
        });
    if (!saved) { return saved; }
    detail::ScenePersistence::saved(document, snapshot);
    return {};
}

Result<void> save_scene(SceneDocument& document, const Project& project)
{
    const auto snapshot = document.snapshot();
    if (!snapshot) { return std::unexpected(snapshot.error()); }
    return save_scene(document, *snapshot, project);
}

Result<void> save_project(const Project& project, const std::filesystem::path& relative_manifest)
{
    const auto path = project.paths().resolve(relative_manifest);
    if (!path) { return std::unexpected(path.error()); }
    const auto text = serialize_project(project.description());
    if (!text) { return std::unexpected(text.error()); }
    return write_file_bytes_atomic(*path, std::as_bytes(std::span{text->data(), text->size()}),
        [&](const std::filesystem::path& temporary) -> Result<void> {
            const auto verified = verify_text(temporary, *text);
            if (!verified) { return verified; }
            const auto parsed = parse_project(*text);
            if (!parsed) { return std::unexpected(parsed.error()); }
            return {};
        });
}
} // namespace dk

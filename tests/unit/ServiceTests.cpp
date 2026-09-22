#include "SceneTestFiles.hpp"
#include <dk/operations/SceneOperations.hpp>

using namespace dk;
namespace
{
struct Session
{
    SceneTestFiles files;
    std::unique_ptr<SceneService> service;
    CommandRegistry commands;
    Session()
    {
        auto s = SceneService::create(files.root);
        REQUIRE(s);
        service = std::move(*s);
        const auto registered = register_scene_commands(commands, *service);
        INFO((registered ? "success" : registered.error().message));
        REQUIRE(registered);
    }
    Json guard()
    {
        auto s = service->state();
        REQUIRE(s);
        return edit_guard_json({s->document_id, s->revision});
    }
    Json call(std::string_view name, Json p = Json::object())
    {
        auto r = commands.execute(name, p);
        INFO((r ? "success" : r.error().message));
        REQUIRE(r);
        return *r;
    }
    Json edit(std::string_view name, Json p = Json::object())
    {
        p["guard"] = guard();
        return call(name, p);
    }
};
} // namespace
TEST_CASE("scene commands create edit hierarchy query and persist")
{
    Session s;
    REQUIRE(s.commands.execute("scene.query").error().code == ErrorCode::invalid_state);
    s.call("scene.new");
    auto parent = s.edit("entity.create")["created_id"];
    auto child = s.edit("entity.create")["created_id"];
    s.edit("entity.set_name", {{"id", child}, {"name", "子实体😀"}});
    s.edit("entity.set_parent", {{"id", child}, {"parent", parent}});
    s.edit("entity.set_transform",
           {{"id", parent},
            {"transform", {{"translation", {3, 2, 1}}, {"rotation", {0, 0, 0, 1}}, {"scale", {2, 1, 1}}}}});
    const auto entity = s.call("entity.get", {{"id", child}});
    REQUIRE(entity["world_matrix"][3] == 3);
    REQUIRE(entity["name"] == "子实体😀");
    const auto before = s.call("scene.query");
    const auto page = s.call("scene.query", {{"limit", 1}});
    REQUIRE(page["entities"].size() == 1);
    REQUIRE(page["has_more"] == true);
    REQUIRE(s.call("scene.query", {{"offset", 2}})["entities"].empty());
    s.edit("scene.save");
    s.edit("project.save", {{"manifest", "project.json"}});
    REQUIRE_FALSE(s.service->state()->dirty);
    s.edit("scene.load", {{"manifest", "project.json"}});
    const auto loaded = s.call("scene.query");
    REQUIRE(loaded["entities"] == before["entities"]);
    REQUIRE(loaded["state"]["scene_id"] == before["state"]["scene_id"]);
    REQUIRE(loaded["state"]["document_id"] != before["state"]["document_id"]);
    REQUIRE(loaded["state"]["revision"] == before["state"]["revision"]);
}
TEST_CASE("stale guards invalid hierarchy and failed replacement preserve scene")
{
    Session s;
    s.call("scene.new");
    auto initial = s.guard();
    auto parent = s.edit("entity.create")["created_id"];
    auto child = s.edit("entity.create")["created_id"];
    s.edit("entity.set_parent", {{"id", child}, {"parent", parent}});
    auto before = s.call("scene.query");
    REQUIRE(s.commands.execute("entity.create", {{"guard", initial}}).error().code == ErrorCode::conflict);
    REQUIRE(s.commands.execute("scene.new").error().code == ErrorCode::conflict);
    REQUIRE_FALSE(s.commands.execute("entity.delete", {{"guard", s.guard()}, {"id", parent}}));
    REQUIRE_FALSE(
        s.commands.execute("entity.set_parent", {{"guard", s.guard()}, {"id", parent}, {"parent", child}}));
    REQUIRE_FALSE(s.commands.execute("scene.load", {{"guard", s.guard()}, {"manifest", "missing.json"}}));
    REQUIRE_FALSE(
        s.commands.execute("entity.create", {{"guard", s.guard()}, {"id", EntityId{}.to_string()}}));
    REQUIRE_FALSE(s.commands.execute(
        "entity.create", {{"guard", {{"document_id", s.guard()["document_id"]}, {"revision", -1}}}}));
    REQUIRE_FALSE(s.commands.execute(
        "entity.create", {{"guard", {{"document_id", s.guard()["document_id"]}, {"revision", 1.0}}}}));
    REQUIRE(s.call("scene.query") == before);
    auto old_guard = s.guard();
    s.edit("scene.save");
    s.edit("project.save", {{"manifest", "project.json"}});
    s.edit("scene.load", {{"manifest", "project.json"}});
    REQUIRE(s.commands.execute("entity.create", {{"guard", old_guard}}).error().code == ErrorCode::conflict);
    REQUIRE(parse_edit_guard({{"document_id", s.guard()["document_id"]},
                              {"revision", std::numeric_limits<std::uint64_t>::max()}}));
}
TEST_CASE("asset edits validate resources before changing the document")
{
    Session s;
    const auto asset = AssetId::generate()->to_string();
    s.call("scene.new", {{"assets", {{{"id", asset}, {"kind", "mesh"}, {"path", "mesh.bin"}}}}});
    auto entity = s.edit("entity.create")["created_id"];
    Json p{{"guard", s.guard()}, {"id", entity}, {"assets", {{{"id", asset}, {"kind", "mesh"}}}}};
    auto before = s.call("scene.query");
    REQUIRE_FALSE(s.commands.execute("entity.set_assets", p));
    REQUIRE(s.call("scene.query") == before);
    s.files.write("mesh.bin", "cpu fixture");
    s.call("entity.set_assets", p);
    REQUIRE(s.call("entity.get", {{"id", entity}})["assets"] == p["assets"]);
    auto no_op = s.guard();
    s.edit("entity.set_assets", {{"id", entity}, {"assets", p["assets"]}});
    REQUIRE(s.guard() == no_op);
    REQUIRE_FALSE(s.commands.execute("project.save", {{"guard", s.guard()}, {"manifest", "scene.json"}}));
}

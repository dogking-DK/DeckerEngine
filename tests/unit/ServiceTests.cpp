#include "SceneTestFiles.hpp"
#include <dk/operations/SceneOperations.hpp>

using namespace dk;
TEST_CASE("snapshot content commits retain save provenance and reject other scenes") {
    SceneTestFiles files;
    auto project = Project::create(files.root, {"Snapshot", "scene.json", {}}); REQUIRE(project);
    auto created = SceneDocument::create(); REQUIRE(created); auto& doc = **created;
    auto original = doc.snapshot(); REQUIRE(original);
    auto staged = SceneDocument::stage(*original); REQUIRE(staged);
    REQUIRE((*staged)->create_entity());
    auto changed = (*staged)->snapshot(); REQUIRE(changed);
    REQUIRE(doc.apply_snapshot(*changed).value()); REQUIRE(doc.revision() == 1);
    REQUIRE(save_scene(doc, *original, *project)); REQUIRE(doc.dirty());
    auto disk = load_scene(*project); REQUIRE(disk); REQUIRE((*disk)->entity_count() == 0);
    REQUIRE(save_scene(doc, *project)); REQUIRE_FALSE(doc.dirty());
    auto other = SceneDocument::create(); REQUIRE(other);
    REQUIRE_FALSE(doc.apply_snapshot(*(*other)->snapshot())); REQUIRE(doc.revision() == 1);
    REQUIRE_FALSE(doc.apply_snapshot(*changed).value()); REQUIRE_FALSE(doc.dirty());
}
namespace
{
struct Session
{
    SceneTestFiles files;
    std::unique_ptr<SceneService> service;
    CommandRegistry commands;
    explicit Session(HistoryLimits limits = {})
    {
        auto s = SceneService::create(files.root, limits);
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

TEST_CASE("transactions commit once undo and redo stable identities and hierarchy")
{
    Session s;
    s.call("scene.new");
    const auto parent = EntityId::generate()->to_string(), child = EntityId::generate()->to_string();
    const Json steps = {
        {{"method", "entity.create"}, {"params", {{"id", parent}}}},
        {{"method", "entity.create"}, {"params", {{"id", child}}}},
        {{"method", "entity.set_parent"}, {"params", {{"id", child}, {"parent", parent}}}},
        {{"method", "entity.set_name"}, {"params", {{"id", child}, {"name", "transaction child"}}}}};
    auto initial = s.guard();
    auto result = s.edit("scene.transaction", {{"commands", steps}});
    REQUIRE(result["state"]["revision"] == 1);
    REQUIRE(result["created_ids"] == Json::array({parent, child, nullptr, nullptr}));
    auto content = s.call("scene.query")["entities"];
    REQUIRE(s.call("history.status")["undo_count"] == 1);
    s.edit("history.undo");
    REQUIRE(s.service->state()->revision == 2);
    REQUIRE(s.service->state()->entity_count == 0);
    REQUIRE(s.commands.execute("entity.create", {{"guard", initial}}).error().code == ErrorCode::conflict);
    s.edit("history.redo");
    REQUIRE(s.service->state()->revision == 3);
    REQUIRE(s.call("scene.query")["entities"] == content);
    s.edit("scene.save");
    REQUIRE_FALSE(s.service->state()->dirty);
    s.edit("history.undo");
    REQUIRE(s.service->state()->dirty);
    s.edit("history.redo");
    REQUIRE(s.service->state()->dirty);
    REQUIRE(s.call("commands.describe", {{"name", "entity.create"}})["undoable"] == true);
}

TEST_CASE("failed and net zero transactions preserve content revision and history")
{
    Session s;
    s.call("scene.new");
    auto id = s.edit("entity.create")["created_id"];
    s.edit("entity.set_name", {{"id", id}, {"name", "new"}});
    s.edit("history.undo");
    const auto before = s.call("scene.query"), history = s.call("history.status");
    Json steps = {{{"method", "entity.set_name"}, {"params", {{"id", id}, {"name", "partial"}}}},
                  {{"method", "entity.set_parent"}, {"params", {{"id", id}, {"parent", id}}}}};
    REQUIRE_FALSE(s.commands.execute("scene.transaction", {{"guard", s.guard()}, {"commands", steps}}));
    for (const auto *name : {"scene.save", "scene.new", "scene.transaction", "history.undo", "entity.get"})
    {
        REQUIRE_FALSE(s.commands.execute(
            "scene.transaction",
            {{"guard", s.guard()}, {"commands", {{{"method", name}, {"params", Json::object()}}}}}));
    }
    REQUIRE_FALSE(s.commands.execute(
        "scene.transaction",
        {{"guard", s.guard()},
         {"commands", {{{"method", "entity.delete"}, {"params", {{"id", id}, {"guard", s.guard()}}}}}}}));
    auto transient = EntityId::generate()->to_string();
    steps = {{{"method", "entity.create"}, {"params", {{"id", transient}}}},
             {{"method", "entity.delete"}, {"params", {{"id", transient}}}}};
    s.edit("scene.transaction", {{"commands", steps}});
    REQUIRE(s.call("scene.query") == before);
    REQUIRE(s.call("history.status") == history);
    s.edit("entity.set_name", {{"id", id}, {"name", "branch"}});
    REQUIRE(s.call("history.status")["redo_count"] == 0);
    REQUIRE_FALSE(s.commands.execute("history.redo", {{"guard", s.guard()}}));
}

TEST_CASE("history obeys budgets and resets on a new document")
{
    Session s({2, 32 * 1024 * 1024});
    s.call("scene.new");
    auto id = s.edit("entity.create")["created_id"];
    s.edit("entity.set_name", {{"id", id}, {"name", "one"}});
    s.edit("entity.set_name", {{"id", id}, {"name", "two"}});
    REQUIRE(s.call("history.status")["undo_count"] == 2);
    s.edit("history.undo");
    s.edit("history.undo");
    REQUIRE(s.service->state()->entity_count == 1);
    REQUIRE_FALSE(s.commands.execute("history.undo", {{"guard", s.guard()}}));
    s.edit("scene.new");
    REQUIRE(s.call("history.status")["logical_bytes"] == 0);
    Session tiny({1, 1});
    tiny.call("scene.new");
    auto before = tiny.call("scene.query");
    REQUIRE_FALSE(tiny.commands.execute("entity.create", {{"guard", tiny.guard()}}));
    REQUIRE(tiny.call("scene.query") == before);
    REQUIRE(tiny.call("history.status")["undo_count"] == 0);
}

TEST_CASE("memory history does not depend on asset files and preserves saved snapshot provenance")
{
    Session s;
    auto asset = AssetId::generate()->to_string();
    s.files.write("mesh.bin", "data");
    s.call("scene.new", {{"assets", {{{"id", asset}, {"kind", "mesh"}, {"path", "mesh.bin"}}}}});
    auto id = s.edit("entity.create")["created_id"];
    s.edit("entity.set_assets", {{"id", id}, {"assets", {{{"id", asset}, {"kind", "mesh"}}}}});
    auto before = s.call("scene.query")["entities"];
    s.edit("entity.set_name", {{"id", id}, {"name", "changed"}});
    REQUIRE(std::filesystem::remove(s.files.root / "mesh.bin"));
    s.edit("history.undo");
    REQUIRE(s.call("scene.query")["entities"] == before);
    REQUIRE_FALSE(s.commands.execute("scene.save", {{"guard", s.guard()}}));
    s.edit("history.redo");
    REQUIRE(s.call("entity.get", {{"id", id}})["name"] == "changed");
    s.files.write("mesh.bin", "restored");
    s.edit("scene.save");
    REQUIRE_FALSE(s.service->state()->dirty);
}

TEST_CASE("a transaction uses one remaining revision and rejects overflow atomically")
{
    Session s;
    s.call("scene.new");
    auto id = s.edit("entity.create")["created_id"];
    s.edit("scene.save");
    s.edit("project.save", {{"manifest", "project.json"}});
    auto project = Project::open(s.files.root, "project.json");
    REQUIRE(project);
    auto snapshot = (*s.service->document())->snapshot();
    REQUIRE(snapshot);
    auto serialized = serialize_scene(*snapshot, *project);
    REQUIRE(serialized);
    auto file = Json::parse(*serialized);
    file["revision"] = std::numeric_limits<std::uint64_t>::max() - 1;
    s.files.write("scene.json", file.dump());
    s.edit("scene.load", {{"manifest", "project.json"}});
    REQUIRE(s.call("history.status")["undo_count"] == 0);
    auto result = s.edit("scene.transaction",
                         {{"commands",
                           {{{"method", "entity.set_name"}, {"params", {{"id", id}, {"name", "first"}}}},
                            {{"method", "entity.set_name"}, {"params", {{"id", id}, {"name", "last"}}}}}}});
    REQUIRE(result["state"]["revision"].get<std::uint64_t>() == std::numeric_limits<std::uint64_t>::max());
    auto before = s.call("scene.query"), history = s.call("history.status");
    REQUIRE_FALSE(s.commands.execute("history.undo", {{"guard", s.guard()}}));
    REQUIRE_FALSE(s.commands.execute("entity.create", {{"guard", s.guard()}}));
    REQUIRE(s.call("scene.query") == before);
    REQUIRE(s.call("history.status") == history);
}

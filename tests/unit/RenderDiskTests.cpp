#include "SceneTestFiles.hpp"
#include <dk/render/DiskScene.hpp>
#include <dk/scene/SceneIO.hpp>
#include <dk/assets/AssetCompiler.hpp>
#include <dk/memory/MemorySystem.hpp>
#include <nlohmann/json.hpp>
#include <catch2/generators/catch_generators.hpp>
using namespace dk;
using namespace dk::render;
namespace {
struct Fixture {
    memory::MemorySystem system=std::move(memory::MemorySystem::create().value());
    memory::ResourceHandle heap=system.create_heap({"disk-tests",memory::DomainCategory::render}).value();
    memory::ThreadContext context{system,heap};
    memory::ExecutionScope scope{context,heap};
    SceneTestFiles files;
    Fixture() { std::filesystem::copy(DK_DISK_FIXTURE_DIR,files.root,std::filesystem::copy_options::recursive); }
    Project project() { return Project::open(files.root,"project.json").value(); }
    nlohmann::json json(std::string_view name) { auto data=read_file_bytes(files.root/name).value(); return nlohmann::json::parse(data); }
};
}
TEST_CASE("disk scene loads immutable project references and deduplicates mesh instances") {
    Fixture f; const auto project=f.project();
    auto loaded=DiskScene::load(f.heap,project); REQUIRE(loaded); REQUIRE(loaded->assets().size()==2);
    CHECK(loaded->scene().entities().size()==4); CHECK(loaded->scene().revision()==1);
    CHECK(loaded->assets()[0]->mesh.id==project.description().assets[0].id);
    CHECK(loaded->assets()[0]->materials[0].alpha_mode==AlphaMode::mask);
    CHECK(loaded->assets()[0]->textures[0].rgba8[3]==std::byte{0});
    CHECK(loaded->scene().entities()[0].world.matrix()(1,3)==0);
    auto source=load_scene(project).value(); auto id=source->snapshot()->entities()[0].id;
    auto local=source->entity(id)->local; local.translation.x()=0.5;
    REQUIRE(source->set_local_transform(id,local)); REQUIRE(save_scene(*source,project));
    auto again=DiskScene::load(f.heap,project); REQUIRE(again); CHECK(again->scene().revision()>loaded->scene().revision());
    CHECK(loaded->scene().entities()[0].world.matrix()(0,3)==0);
    CHECK(again->scene().entities()[0].world.matrix()(0,3)==0.5);
    CHECK_FALSE(std::filesystem::exists(f.files.root/"assets/mask.gltf.meta"));
    CHECK_FALSE(std::filesystem::exists(f.files.root/".cache"));
    f.heap.begin_close(); CHECK_FALSE(DiskScene::load(f.heap,project)); CHECK(loaded->assets()[0]->mesh.primitives[0].positions.size()==4);
}
TEST_CASE("disk scene consumes M4 artifacts and enforces persistent identity") {
    Fixture f; auto paths=ProjectPaths::create(f.files.root).value();
    auto compiled=compile_asset(paths,{"assets/mask.gltf","compiled",2}); REQUIRE(compiled);
    auto document=f.json("scene.json"); auto description=f.project().description();
    const auto old_id=description.assets[0].id.to_string();
    description.assets[0].id=compiled->root_id;
    for (auto& e : document["entities"]) for (auto& ref : e["components"]["dk.AssetReferences"]["items"])
        if (ref["id"]==old_id) ref["id"]=compiled->root_id.to_string();
    f.files.write("scene.json",document.dump());
    auto project=Project::create(f.files.root,description).value();
    auto direct=DiskScene::load(f.heap,project); REQUIRE(direct);
    CHECK(direct->assets()[0]->mesh.primitives[0].positions[0].x()==-2);
    const auto sub_id=direct->assets()[0]->textures[0].id;
    description.assets[0].path="compiled/manifest.json"; project=Project::create(f.files.root,description).value();
    auto artifact=DiskScene::load(f.heap,project); REQUIRE(artifact);
    CHECK(artifact->assets()[0]->mesh.id==compiled->root_id); CHECK(artifact->assets()[0]->textures[0].id==sub_id);
    CHECK(artifact->assets()[0]->unit_scale==2);
    auto bad=f.json("compiled/manifest.json"); bad["data"]["digest"]="00000000000000000000000000000000";
    f.files.write("compiled/manifest.json",bad.dump()); CHECK_FALSE(DiskScene::load(f.heap,project));
    CHECK(artifact->assets()[0]->textures[0].id==sub_id);
}
TEST_CASE("disk scene failure leaves old candidate and source bytes intact") {
    const int scenario=GENERATE(0,1,2,3,4,5,6,7,8,9);
    Fixture f; auto project=f.project(); auto old=DiskScene::load(f.heap,project); REQUIRE(old);
    DiskSceneOptions options;
    switch (scenario) {
    case 0: f.files.write("assets/mask.gltf","broken"); break;
    case 1: f.files.write("assets/mask.gltf.meta","invalid"); break;
    case 2: REQUIRE(std::filesystem::remove(f.files.root/"assets/mask.png")); break;
    case 3: { auto j=f.json("assets/mask.gltf"); j["materials"][0]["alphaMode"]="BLEND"; f.files.write("assets/mask.gltf",j.dump()); break; }
    case 4: options.max_assets=1; break;
    case 5: options.cpu_bytes=1; break;
    case 6: options.profile=static_cast<GltfImportProfile>(99); break;
    case 7: f.files.write(".decker/asset-operations/unrecovered.json","pending"); break;
    case 8: REQUIRE(compile_asset(project.paths(),{"assets/mask.gltf","compiled"})); break; // New meta mesh ID differs.
    case 9: { auto j=f.json("scene.json"); j["entities"][0]["components"]["dk.AssetReferences"]["items"][0]["id"]=AssetId::generate()->to_string(); f.files.write("scene.json",j.dump()); break; }
    }
    const auto before=read_file_bytes(f.files.root/"scene.json").value();
    REQUIRE_FALSE(DiskScene::load(f.heap,project,options));
    CHECK(read_file_bytes(f.files.root/"scene.json").value()==before);
    CHECK(old->assets().size()==2); CHECK(old->assets()[0]->textures[0].rgba8[7]==std::byte{255});
    CHECK(old->scene().revision()==1);
}
TEST_CASE("empty disk scene exposes no scene or GPU cache") {
    DiskScene scene; CHECK_FALSE(scene); CHECK_FALSE(scene.scene()); CHECK(scene.assets().empty());
}

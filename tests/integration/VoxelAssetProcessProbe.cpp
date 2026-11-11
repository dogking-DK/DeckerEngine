#include "../VoxelAssetSupport.hpp"
#include <dk/assets/AssetCache.hpp>
#include <dk/scene/SceneIO.hpp>
#include <dk/memory/MemorySystem.hpp>
#include <dk/memory/Context.hpp>
#include <cstdio>
namespace {
using namespace dk;
template<class T,class E> T take(std::expected<T,E> r) { if(!r) { if constexpr(std::is_same_v<E,Error>) throw std::runtime_error(r.error().message); else throw std::bad_alloc{}; } return std::move(*r); }
void check(Result<void> r) { if(!r) throw std::runtime_error(r.error().message); }
void require(bool v,const char* text) { if(!v) throw std::runtime_error(text); }
void run(std::string_view mode,const std::filesystem::path& root) {
    const auto paths=take(ProjectPaths::create(root));
    if(mode=="write") {
        voxel_fixture::write(root);
        const auto observed=take(assets::import_voxel(paths,{"assets/solid.dkvoxel"})); const auto& stats=observed.voxel->grid->statistics();
        std::printf("source_tree_bytes=%llu normalized_tree_bytes=%llu nano_bytes=%llu\n",
            static_cast<unsigned long long>(stats.source_tree_bytes),static_cast<unsigned long long>(stats.normalized_tree_bytes),static_cast<unsigned long long>(stats.nano_bytes));
        for(const auto& message:observed.diagnostics) std::printf("%s\n",message.c_str());
    }
    const auto result=take(compile_cached_asset(paths,{"assets/solid.dkvoxel"})); const auto id=root_asset_id(result.artifact.data);
    require(result.artifact.data.voxel && result.artifact.data.voxel->grid->value({-8,0,0})==1,"Reloaded voxel value differs");
    if(mode=="write") {
        require(!result.cache_hit,"First process unexpectedly hit cache");
        auto project=take(Project::create(root,{"Voxel process","scene.json",{{id,AssetKind::voxel,"assets/solid.dkvoxel"}}}));
        auto scene=take(SceneDocument::create()); const auto entity=take(EntityId::generate());
        check(scene->create_entity(entity)); check(scene->set_asset_references(entity,{{id,AssetKind::voxel}}));
        check(save_project(project,"project.json")); check(save_scene(*scene,project));
        voxel_fixture::write_text(root/"expected.json",{{"id",id.to_string()},{"key",std::string{result.key}},{"scene",scene->id().to_string()}});
    } else {
        require(mode=="read" && result.cache_hit,"Second process did not hit existing cache");
        const auto expected=voxel_fixture::Json::parse(voxel_fixture::read(root/"expected.json"));
        auto project=take(Project::open(root,"project.json")); auto scene=take(load_scene(project));
        require(id.to_string()==expected["id"].get<std::string>() && std::string{result.key}==expected["key"].get<std::string>() && scene->id().to_string()==expected["scene"].get<std::string>(),"Process identity/key/Scene changed");
        const auto snapshot=take(scene->snapshot()); const auto serialized=take(serialize_scene(snapshot,project));
        require(serialized.find("voxel")!=serialized.npos && serialized.find(id.to_string())!=serialized.npos,"Scene voxel reference missing");
    }
    std::printf("mode=%.*s id=%s key=%s hit=%d value=1 CPU-only\n",int(mode.size()),mode.data(),id.to_string().c_str(),result.key.c_str(),result.cache_hit);
}
}
#ifdef _WIN32
int wmain(int argc,wchar_t** argv) {
#else
int main(int argc,char** argv) {
#endif
    try {
        if(argc!=3) throw std::runtime_error("Expected write/read and project root");
        auto memory=take(dk::memory::MemorySystem::create()); auto heap=take(memory.create_heap({"voxel-process",dk::memory::DomainCategory::assets}));
        {
            dk::memory::ThreadContext context{memory}; dk::memory::ExecutionScope scope{context,heap};
#ifdef _WIN32
            run(take(dk::path_to_utf8(std::filesystem::path{argv[1]})),std::filesystem::path{argv[2]});
#else
            run(argv[1],take(dk::path_from_utf8(argv[2])));
#endif
        }
        require(!heap.snapshot().live_allocations && memory.try_close().closed(),"CPU process leaked owned resources"); return 0;
    } catch(const std::exception& e) { std::fprintf(stderr,"voxel process failed: %s\n",e.what()); return 1; }
}

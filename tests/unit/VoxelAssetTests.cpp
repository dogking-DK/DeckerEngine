#include "AssetTestSupport.hpp"
#include "../VoxelAssetSupport.hpp"
#include "AssetCacheInternal.hpp"
#include "CpuArtifactInternal.hpp"
#include <dk/assets/AssetCompiler.hpp>
#include <dk/scene/SceneIO.hpp>
#include <openvdb/io/File.h>
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4702) // NanoVDB toGridType<T>, as in VoxelSparseTests.
#endif
#include <nanovdb/tools/GridChecksum.h>
#include <nanovdb/HostBuffer.h>
#ifdef _MSC_VER
#pragma warning(pop)
#endif
#include <cstring>

namespace {
using namespace dk;
using namespace voxel_fixture;
struct Fixture {
    AssetTestMemory memory; SceneTestFiles files;
    ProjectPaths paths=ProjectPaths::create(files.root).value();
    Fixture() { voxel_fixture::write(files.root); }
    std::filesystem::path source() const { return files.root/"assets/solid.dkvoxel"; }
    std::filesystem::path vdb() const { return files.root/"assets/grid.vdb"; }
    std::filesystem::path meta() const { auto p=source(); p+=".meta"; return p; }
    std::filesystem::path product(std::string_view name="product") const { return files.root/path_from_utf8(name).value(); }
    void save(const Json& j) { write_text(source(),j); }
};
template<class T> T checked(Result<T> value) { INFO((value ? "success" : value.error().message)); REQUIRE(value); return std::move(*value); }
std::string current(const Fixture& f,AssetId id) { return read(f.files.root/".decker/cache/assets/v1/current"/(id.to_string()+".json")); }
}
TEST_CASE("voxel file artifact and Scene roundtrip preserve identity map and sparse values", "[voxel-assets]") {
    Fixture f; auto imported=checked(assets::import_voxel(f.paths,{"assets/solid.dkvoxel"}));
    REQUIRE(imported.voxel); const auto& grid=*imported.voxel->grid;
    CHECK(grid.value({-8,0,0})==1); CHECK(grid.value({0,0,0})==2); CHECK(grid.value({1,1,1})==0);
    CHECK(grid.corner_to_local().transform_point({0,0,0}).value()==Vec3d::Zero());
    CHECK(grid.nearest({{-2,.2,.2},{1,0,0}})->hit->distance==1);
    auto compiled=checked(compile_asset(f.paths,{"assets/solid.dkvoxel","product",2}));
    auto loaded=checked(load_cpu_artifact(f.product())); REQUIRE(loaded.data.voxel); CHECK(root_asset_id(loaded.data)==compiled.root_id);
    CHECK(loaded.data.voxel->grid->value({-8,0,0})==1); CHECK(loaded.data.voxel->grid->nearest({{-3,.2,.2},{1,0,0}})->hit->distance==1);
    auto project=checked(Project::create(f.files.root,{"voxel CPU","scene.json",{{compiled.root_id,AssetKind::voxel,"assets/solid.dkvoxel"}}}));
    auto scene=checked(SceneDocument::create()); const auto entity=AssetId::generate().value().to_string(); const auto id=EntityId::parse(entity).value();
    REQUIRE(scene->create_entity(id)); REQUIRE(scene->set_asset_references(id,{{compiled.root_id,AssetKind::voxel}}));
    REQUIRE(save_project(project,"project.json")); REQUIRE(save_scene(*scene,project));
    auto reopened=checked(Project::open(f.files.root,"project.json")); auto restored=checked(load_scene(reopened));
    CHECK(restored->id()==scene->id()); CHECK(restored->revision()==scene->revision()); CHECK_FALSE(restored->dirty());
    CHECK(checked(serialize_scene(restored->snapshot().value(),reopened))==checked(serialize_scene(scene->snapshot().value(),project)));
    auto text=Json::parse(read(f.files.root/"scene.json")); text["entities"][0]["components"]["dk.AssetReferences"]["items"][0]["kind"]="future-voxel";
    f.files.write("scene.json",text.dump()); const auto before=restored.get(); REQUIRE_FALSE(reload_scene(restored,reopened)); CHECK(restored.get()==before);
    auto catalog=checked(AssetCatalog::create(f.files.root)); const auto candidate=checked(catalog.prepare_registration(catalog.guard(),{"assets/solid.dkvoxel",{}, {},MissingMetaPolicy::reject}));
    CHECK(candidate.metadata().root_id==compiled.root_id);
}
TEST_CASE("voxel source reads standard seekable VDB file archives from owned bytes", "[voxel-assets]") {
    Fixture f;
    const auto path=f.files.root.parent_path()/"standard.vdb";
    { openvdb::io::File archive(path.string()); archive.write(openvdb::GridPtrVec{grid()}); archive.close(); }
    std::filesystem::copy_file(path,f.vdb(),std::filesystem::copy_options::overwrite_existing);
    auto imported=checked(assets::import_voxel(f.paths,{"assets/solid.dkvoxel"})); CHECK(imported.voxel->grid->value({-8,0,0})==1);
    CHECK(imported.inputs[1].bytes.size()==std::filesystem::file_size(path));
}
TEST_CASE("voxel source rejects ambiguous semantics bad grids bytes budgets and closing", "[voxel-assets]") {
    Fixture f; const auto original=read(f.vdb());
    for(unsigned scenario=0;scenario<23;++scenario) {
        INFO(scenario); f.files.write("assets/grid.vdb",original); auto j=description(); assets::VoxelImportRequest request{"assets/solid.dkvoxel"};
        switch(scenario) {
        case 0: j.erase("semantics"); break;
        case 1: j["semantics"]="density"; break;
        case 2: j["grid"]="missing"; break;
        case 3: j["grid"]="density"; break;
        case 4: j["palette"][0]["base_color"][0]=1.1; break;
        case 5: j["domain"]["minimum"][0]=0; break;
        case 6: j["vdb"]="../../outside.vdb"; break;
        case 7: j["version"]=2; break;
        case 8: f.files.write("assets/grid.vdb",original.substr(0,original.size()/2)); break;
        case 9: f.files.write("assets/grid.vdb",original+"tail"); break;
        case 10: { auto g=grid(); g->setGridClass(openvdb::GRID_LEVEL_SET); write_vdb(f.vdb(),g); break; }
        case 11: { auto g=openvdb::Int32Grid::create(1); g->setName("solid"); write_vdb(f.vdb(),g); break; }
        case 12: { auto g=grid(); g->tree().setValue({2,2,2},-1); write_vdb(f.vdb(),g); break; }
        case 13: request.limits.description_bytes=1; break;
        case 14: request.limits.file_bytes=1; break;
        case 15: request.limits.decoded_tree_bytes=1; break;
        case 16: request.limits.build.maximum_nano_bytes=1; break;
        case 17: request.limits.live_payload_bytes=1; break;
        case 18: j["domain"]["minimum"][0]=UINT64_MAX; break;
        case 19: request.limits.input_bytes=1; break;
        case 20: request.unit_scale=0; break;
        case 21: { auto g=grid(); g->tree().setValue({2,2,2},3); write_vdb(f.vdb(),g); break; }
        case 22: { auto g=grid(); g->setTransform(openvdb::math::Transform::createFrustumTransform(openvdb::BBoxd({0,0,0},{10,10,10}),.5,10,1)); write_vdb(f.vdb(),g); break; }
        }
        f.save(j); CHECK_FALSE(assets::import_voxel(f.paths,request));
    }
    f.files.write("assets/grid.vdb",original); f.save(description());
    const auto good=checked(assets::import_voxel(f.paths,{"assets/solid.dkvoxel"}));
    auto duplicate=read(f.source()); duplicate.insert(duplicate.find("\"version\""),"\"version\":1,"); f.files.write("assets/solid.dkvoxel",duplicate);
    CHECK_FALSE(assets::import_voxel(f.paths,{"assets/solid.dkvoxel"})); f.save(description());
    std::stop_source stop; stop.request_stop(); CHECK_FALSE(assets::import_voxel(f.paths,{"assets/solid.dkvoxel",{},1,{},stop.get_token()}));
    CHECK_FALSE(f.memory.system.try_close().closed()); CHECK_FALSE(assets::import_voxel(f.paths,{"assets/solid.dkvoxel"}));
    CHECK(good.voxel->grid->value({-8,0,0})==1);
}
TEST_CASE("voxel artifact loader rejects corrupt layout addresses values and trailing data", "[voxel-assets]") {
    Fixture f; checked(compile_asset(f.paths,{"assets/solid.dkvoxel","product"}));
    const auto original=read(f.product()/"manifest.json"); const auto payload=read(f.product()/"data.bin");
    for(unsigned scenario=0;scenario<8;++scenario) {
        auto j=Json::parse(original); std::string bytes=payload; INFO(scenario);
        switch(scenario) {
        case 0: j["layout"]["version"]=1; break;
        case 1: j["layout"]["builder"]="future"; break;
        case 2: j["palette"][0]["metallic"]=-1; break;
        case 3: j["corner_to_local"][0]=0; break;
        case 4: j["domain"]["minimum"][0]=0; break;
        case 5: bytes.pop_back(); break;
        case 6: bytes+="tail"; break;
        case 7: j["semantics"]="sdf"; break;
        }
        f.files.write("product/manifest.json",j.dump()); f.files.write("product/data.bin",bytes); CHECK_FALSE(load_cpu_artifact(f.product()));
    }
    f.files.write("product/manifest.json",original); f.files.write("product/data.bin",payload);
    const auto good=checked(load_cpu_artifact(f.product())); const auto& source=*good.data.voxel->grid;
    using Grid=nanovdb::NanoGrid<std::int32_t>;
    for(unsigned scenario=0;scenario<9;++scenario) {
        INFO(scenario); auto buffer=nanovdb::HostBuffer::create(payload.size()); std::memcpy(buffer.data(),payload.data(),payload.size()); auto* g=reinterpret_cast<Grid*>(buffer.data());
        auto* tree=g->tree().data(); auto* root=g->tree().root().data();
        switch(scenario) {
        case 0: tree->mNodeOffset[3]=INT64_MAX; break;
        case 1: root->mTableSize=UINT32_MAX; break;
        case 2: tree->mNodeCount[0]=UINT32_MAX; break;
        case 3: root->tile(0)->child=INT64_MAX; break;
        case 4: g->tree().getFirstNode<0>()->data()->mValues[0]=-1; break;
        case 5: g->tree().getFirstNode<1>()->data()->mTable[0].child=INT64_MAX; g->tree().getFirstNode<1>()->data()->mChildMask.setOn(0); break;
        case 6: g->mGridCount=2; break;
        case 7: g->tree().getFirstNode<0>()->data()->mBBoxMin[0]+=8; break;
        case 8: g->mChecksum.disable(); break;
        }
        // Full v32.7 checksum operates on bounded contiguous bytes; updated checksums must not bypass structural checks.
        if(scenario!=8) nanovdb::tools::updateChecksum(g,nanovdb::CheckMode::Full);
        CHECK_FALSE(geometry::voxels::load_sparse_grid({static_cast<const std::byte*>(buffer.data()),payload.size()},source.domain(),source.corner_to_local(),2));
    }
    CHECK(source.value({-8,0,0})==1);
}
TEST_CASE("voxel cache content changes preserve IDs snapshots and rebuild incompatible layouts", "[voxel-assets]") {
    Fixture f; auto first=checked(compile_cached_asset(f.paths,{"assets/solid.dkvoxel"})); const auto id=root_asset_id(first.artifact.data);
    const auto before=current(f,id); const auto meta=read(f.meta());
    const dk::asset_detail::CacheHook deny=[](auto step)->Result<void> { if(step==dk::asset_detail::CacheStep::import) return std::unexpected(Error{ErrorCode::internal_error,"Unexpected import on cache hit"}); return {}; };
    { dk::asset_detail::ScopedCacheHook hook{deny}; const auto hit=checked(compile_cached_asset(f.paths,{"assets/solid.dkvoxel"})); CHECK(hit.cache_hit); CHECK(hit.key==first.key); }
    CHECK(current(f,id)==before); CHECK(read(f.meta())==meta);
    const auto size=std::filesystem::file_size(f.source()); const auto time=std::filesystem::last_write_time(f.source());
    auto j=description(); j["palette"][0]["base_color"][0]=.375; f.save(j); std::filesystem::last_write_time(f.source(),time); REQUIRE(std::filesystem::file_size(f.source())==size);
    auto palette=checked(compile_cached_asset(f.paths,{"assets/solid.dkvoxel"})); CHECK_FALSE(palette.cache_hit); CHECK(palette.key!=first.key); CHECK(root_asset_id(palette.artifact.data)==id);
    CHECK(first.artifact.data.voxel->palette[0].base_color.x()==.125f); CHECK(palette.artifact.data.voxel->palette[0].base_color.x()==.375f);
    const auto file_time=std::filesystem::last_write_time(f.vdb()); write_vdb(f.vdb(),grid(2)); std::filesystem::last_write_time(f.vdb(),file_time);
    auto changed=checked(compile_cached_asset(f.paths,{"assets/solid.dkvoxel"})); CHECK(changed.key!=palette.key); CHECK(changed.artifact.data.voxel->grid->value({-8,0,0})==2); CHECK(first.artifact.data.voxel->grid->value({-8,0,0})==1);
    j["grid"]="alternate"; f.save(j); auto named=checked(compile_cached_asset(f.paths,{"assets/solid.dkvoxel",2})); CHECK(named.key!=changed.key); CHECK(root_asset_id(named.artifact.data)==id);
    const auto folder=f.files.root/path_from_utf8(named.directory).value(); auto manifest=Json::parse(read(folder/"manifest.json")); manifest["layout"]["version"]=1;
    f.files.write(path_from_utf8(std::string{named.directory}+"/manifest.json").value(),manifest.dump());
    auto entry=Json::parse(read(folder/"entry.json")); const auto raw=read(folder/"manifest.json");
    entry["manifest_digest"]=dk::asset_detail::content_digest(std::as_bytes(std::span{raw.data(),raw.size()})).hex();
    f.files.write(path_from_utf8(std::string{named.directory}+"/entry.json").value(),entry.dump());
    auto rebuilt=checked(compile_cached_asset(f.paths,{"assets/solid.dkvoxel"})); CHECK_FALSE(rebuilt.cache_hit); CHECK(rebuilt.directory!=named.directory); CHECK(root_asset_id(rebuilt.artifact.data)==id); CHECK(std::filesystem::exists(folder));
}
TEST_CASE("voxel cache failure cancellation and registration preserve prior publication", "[voxel-assets]") {
    Fixture f; auto first=checked(compile_cached_asset(f.paths,{"assets/solid.dkvoxel"})); const auto id=root_asset_id(first.artifact.data); const auto before=current(f,id),meta=read(f.meta());
    auto j=description(); j["palette"][0]["roughness"]=.25; f.save(j);
    using namespace dk::asset_detail;
    for(auto step:{CacheStep::validate_inputs,CacheStep::publish,CacheStep::metadata,CacheStep::current}) {
        const CacheHook failure=[&](auto s)->Result<void> { if(s==step) return std::unexpected(Error{ErrorCode::io_error,"Injected voxel publication failure"}); return {}; };
        ScopedCacheHook hook{failure}; auto rejected=compile_cached_asset(f.paths,{"assets/solid.dkvoxel"}); CHECK_FALSE(rejected); CHECK(current(f,id)==before); CHECK(read(f.meta())==meta);
    }
    { auto candidate=checked(prepare_cached_asset(f.paths,{"assets/solid.dkvoxel"})); CHECK(current(f,id)==before); }
    { auto candidate=checked(prepare_cached_asset(f.paths,{"assets/solid.dkvoxel"})); write_vdb(f.vdb(),grid(2)); CHECK_FALSE(candidate.publish()); CHECK(current(f,id)==before); }
    CHECK(current(f,id)==before); std::stop_source stop; stop.request_stop(); CHECK_FALSE(prepare_cached_asset(f.paths,{"assets/solid.dkvoxel"},stop.get_token()));
    auto published=checked(compile_cached_asset(f.paths,{"assets/solid.dkvoxel"})); CHECK(current(f,id)!=before); CHECK(first.artifact.data.voxel->palette[0].roughness==1);
    const AssetOutputSpec output{"voxel/0",AssetKind::voxel}; auto catalog=checked(AssetCatalog::create(f.files.root));
    const auto candidate=checked(catalog.prepare_registration(catalog.guard(),{"assets/solid.dkvoxel",{&output,1},{},MissingMetaPolicy::reject})); CHECK(candidate.metadata().root_id==id);
}
TEST_CASE("voxel raw reload supports empty root tiles and negative sparse hierarchies", "[voxel-assets]") {
    AssetTestMemory memory; using namespace dk::assets;
    const std::array blocks{VoxelBlock{{-4096,0,0},4096,1,false},VoxelBlock{{0,0,0},4096,2,true}};
    for(unsigned scenario=0;scenario<5;++scenario) {
        VoxelBuildOptions options; options.palette_size=2; options.domain=geometry::voxels::Domain{{-4096,0,0},{4095,4095,4095}};
        Vector<VoxelBlock> values;
        if(scenario==1) values.assign(blocks.begin(),blocks.end());
        if(scenario==2) values={{{-4000,0,0},1,1,false},{{0,0,0},1,2,true},{{129,129,129},1,1,true}};
        if(scenario==3) values={{{-4000,0,0},1,0,true}};
        if(scenario==4) values={{{0,0,0},1,1,true},{{4096,0,0},1,0,true}};
        const auto built=checked(build_voxel_grid(values,options));
        const auto restored=checked(geometry::voxels::load_sparse_grid(built.bytes(),built.domain(),built.corner_to_local(),2));
        for(auto c:{geometry::voxels::Coord{-4000,0,0},{0,0,0},{129,129,129},{4095,4095,4095}}) CHECK(restored.value(c)==built.value(c));
        const geometry::Ray ray{{-5000,0,0},{1,0,0}}; const auto a=checked(built.nearest(ray)),b=checked(restored.nearest(ray));
        CHECK(a.hit.has_value()==b.hit.has_value()); if(a.hit && b.hit) CHECK(a.hit->distance==b.hit->distance);
    }
}
TEST_CASE("voxel registration and same directory rename retain identity and VDB dependency", "[voxel-assets]") {
    Fixture f; auto catalog=checked(AssetCatalog::create(f.files.root));
    Json manifest{{"format","DeckerProject"},{"version",1},{"name","voxel catalog"},{"scene","scene.json"},{"assets",Json::array()}};
    const auto before=manifest.dump(); f.files.write("project.json",before);
    const AssetOutputSpec output{"voxel/0",AssetKind::voxel};
    auto candidate=checked(catalog.prepare_registration(catalog.guard(),{"assets/solid.dkvoxel",{&output,1},{},MissingMetaPolicy::create_or_adopt}));
    const auto id=candidate.metadata().root_id;
    manifest["assets"].push_back({{"id",id.to_string()},{"kind","voxel"},{"path","assets/solid.dkvoxel"}}); const auto after=manifest.dump();
    REQUIRE(catalog.commit_registration(std::move(candidate),{"project.json",before,after}));
    CHECK(catalog.records().size()==1); CHECK(catalog.records()[0].kind==AssetKind::voxel);
    auto first=checked(compile_cached_asset(f.paths,{"assets/solid.dkvoxel"})); CHECK(root_asset_id(first.artifact.data)==id);
    auto rename=checked(catalog.prepare_rename(catalog.guard(),"assets/solid.dkvoxel","assets/renamed.dkvoxel"));
    manifest["assets"][0]["path"]="assets/renamed.dkvoxel"; const auto renamed=manifest.dump();
    REQUIRE(catalog.commit_rename(std::move(rename),{"project.json",after,renamed}));
    auto next=checked(compile_cached_asset(f.paths,{"assets/renamed.dkvoxel"})); CHECK(root_asset_id(next.artifact.data)==id); CHECK(next.key!=first.key);
    CHECK(next.artifact.data.voxel->grid->value({-8,0,0})==1); CHECK(first.artifact.data.voxel->grid->value({-8,0,0})==1);
    CHECK(Project::open(f.files.root,"project.json"));
}

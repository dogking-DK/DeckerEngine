#include <dk/render/DiskScene.hpp>
#include <dk/assets/CpuArtifact.hpp>
#include <dk/assets/Metadata.hpp>
#include <dk/assets/Persistence.hpp>
#include <dk/scene/SceneIO.hpp>
#include <dk/io/File.hpp>
#include <dk/memory/Context.hpp>
#include <dk/memory/SmartPtr.hpp>
#include <dk/profiling/Profiler.hpp>
#include <algorithm>
#include <new>
#ifdef DK_DISK_VOXELS
#include <dk/assets/VoxelImporter.hpp>
#endif

namespace dk::render {
namespace detail {
struct DiskSceneState {
    explicit DiskSceneState(memory::ResourceHandle resource)
        : heap(resource), assets(0,memory::Allocator<std::shared_ptr<const CpuAsset>>{resource}) {}
    memory::ResourceHandle heap;
    RenderScene scene;
    Vector<std::shared_ptr<const CpuAsset>> assets;
    std::size_t cpu_bytes=0;
};
}
namespace {
template<class T> T take(Result<T> value) { if (!value) throw std::move(value.error()); return std::move(*value); }
void check(Result<void> value) { if (!value) throw std::move(value.error()); }
void require(bool condition, const char* message, ErrorCode code=ErrorCode::invalid_argument) {
    if (!condition) throw Error{code,message};
}
std::size_t bytes(const CpuAsset& asset, std::size_t remaining) {
    std::size_t result=0;
    auto add=[&](std::size_t count,std::size_t stride) {
        require(count <= (remaining-result)/stride,"disk scene CPU payload budget exceeded"); result+=count*stride;
    };
    for (const auto& p : asset.mesh.primitives) {
        add(p.positions.size(),sizeof(Vec3f)); add(p.normals.size(),sizeof(Vec3f));
        add(p.texcoords.size(),sizeof(Vec2f)); add(p.indices.size(),sizeof(std::uint32_t));
    }
    for (const auto& t : asset.textures) add(t.rgba8.size(),1);
#ifdef DK_DISK_VOXELS
    if(asset.voxel) { add(asset.voxel->grid->bytes().size(),1); add(asset.voxel->palette.size(),sizeof(VoxelMaterialData)); }
#endif
    return result;
}
CpuAsset load_asset(const Project& project,AssetReference reference,const DiskSceneOptions& options) {
    const auto path=take(project.resolve_asset(reference));
    const auto extension=path.extension();
    if (path.filename()=="manifest.json") {
        auto artifact=take(load_cpu_artifact(path.parent_path()));
        require(root_asset_id(artifact.data)==reference.id && bool(artifact.data.voxel)==(reference.kind==AssetKind::voxel),"CPU artifact ID/kind differs from Project",ErrorCode::conflict);
        return std::move(artifact.data);
    }
    const bool voxel=reference.kind==AssetKind::voxel;
    require(voxel ? extension==".dkvoxel" : (extension==".gltf" || extension==".glb"),"asset path must match mesh glTF/GLB or voxel .dkvoxel",ErrorCode::not_supported);
    const auto relative=take(path_to_utf8(path.lexically_relative(project.paths().root())));
    Vector<OutputIdentity> identities;
    double scale=1;
    auto meta_path=path; meta_path+=".meta";
    std::error_code ec;
    const bool meta_exists=std::filesystem::exists(meta_path,ec);
    require(!ec,"cannot inspect asset metadata",ErrorCode::io_error);
    if (meta_exists) {
        auto data=take(read_file_bytes(meta_path,asset_meta_byte_limit));
        auto meta=take(parse_asset_meta(std::string_view{reinterpret_cast<const char*>(data.data()),data.size()}));
        scale=meta.unit_scale;
        for (const auto& output : meta.outputs) identities.push_back({output.key,output.id,output.kind});
        auto root=std::ranges::find(identities,std::string_view{voxel ? "voxel/0" : "mesh/0"},[](const auto& output){return std::string_view{output.key};});
        require(root!=identities.end() && root->id==reference.id && root->kind==reference.kind,
            "source metadata root ID/kind differs from Project",ErrorCode::conflict);
    } else identities.push_back({String{voxel ? "voxel/0" : "mesh/0"},reference.id,reference.kind});
    if(voxel) {
#ifdef DK_DISK_VOXELS
        assets::VoxelImportRequest request{relative,identities,scale}; request.stop=options.stop;
        request.limits.build.maximum_nano_bytes=std::min(request.limits.build.maximum_nano_bytes,std::uint64_t(options.cpu_bytes));
        auto imported=take(assets::import_voxel(project.paths(),request));
        return std::move(static_cast<CpuAsset&>(imported));
#else
        throw Error{ErrorCode::not_supported,"DiskScene voxel support was not built"};
#endif
    }
    GltfImportRequest request{relative,identities,scale}; request.profile=options.profile; request.stop=options.stop; request.limits=options.import_limits;
    auto imported=take(import_gltf(project.paths(),request));
    return std::move(static_cast<CpuAsset&>(imported));
}
}
Result<DiskScene> DiskScene::load(memory::ResourceHandle heap,const Project& project,const DiskSceneOptions& options) {
    auto document=load_scene(project); if (!document) return std::unexpected(document.error());
    auto snapshot=(*document)->snapshot(); if (!snapshot) return std::unexpected(snapshot.error());
    return load_snapshot(heap,project,*snapshot,options);
}
Result<DiskScene> DiskScene::load_snapshot(memory::ResourceHandle heap,const Project& project,const SceneSnapshot& snapshot,const DiskSceneOptions& options) {
    DK_PROFILE_ZONE("render.load_disk_scene");
    if (!heap || heap.state()!=memory::ResourceState::open)
        return std::unexpected(Error{ErrorCode::invalid_state,"disk scene requires open memory"});
    if (!options.max_assets || !options.cpu_bytes ||
        (options.profile!=GltfImportProfile::strict && options.profile!=GltfImportProfile::unlit_preview))
        return std::unexpected(Error{ErrorCode::invalid_argument,"invalid disk scene limits/profile"});
    try {
        memory::DomainScope domain{heap};
        check(check_asset_operations(project.paths().root()));
        require(!options.stop.stop_requested(),"disk scene load cancelled",ErrorCode::invalid_state);
        auto candidate=memory::make_shared_in<detail::DiskSceneState>(heap,heap);
        candidate->scene=take(RenderScene::extract(heap,snapshot,{},options.unlit_inspection));
        std::size_t used=0;
        for (std::size_t i=0;i<candidate->scene.entities().size();++i) {
            for (auto reference : take(candidate->scene.assets(i))) {
#ifndef DK_DISK_VOXELS
                require(reference.kind!=AssetKind::voxel,"DiskScene voxel support was not built",ErrorCode::not_supported);
#endif
                if (reference.kind!=AssetKind::mesh && reference.kind!=AssetKind::voxel) continue;
                if (std::ranges::any_of(candidate->assets,[&](const auto& asset){return root_asset_id(*asset)==reference.id;})) continue;
                require(candidate->assets.size()<options.max_assets,"disk scene asset count exceeds limit");
                require(!options.stop.stop_requested(),"disk scene load cancelled",ErrorCode::invalid_state);
                auto loaded=load_asset(project,reference,options);
                if(!loaded.voxel) check(validate_gpu_asset(loaded));
                for (const auto& material : loaded.materials)
                    require(material.alpha_mode!=AlphaMode::blend,"disk scene alpha blend is not supported",ErrorCode::not_supported);
                used+=bytes(loaded,options.cpu_bytes-used);
                candidate->assets.push_back(memory::make_shared_in<CpuAsset>(heap,std::move(loaded)));
            }
        }
        candidate->cpu_bytes=used;
        return DiskScene{std::move(candidate)};
    } catch (Error& error) { return std::unexpected(error.with_context("DiskScene.load")); }
      catch (const std::bad_alloc&) { return std::unexpected(Error{ErrorCode::internal_error,"disk scene allocation failed; candidate discarded"}); }
}
RenderScene DiskScene::scene() const noexcept { return state_ ? state_->scene : RenderScene{}; }
std::span<const std::shared_ptr<const CpuAsset>> DiskScene::assets() const noexcept {
    return state_ ? std::span<const std::shared_ptr<const CpuAsset>>{state_->assets} : std::span<const std::shared_ptr<const CpuAsset>>{};
}
Result<GpuAssets> DiskScene::upload(graphics::SubmissionQueue& queue) const {
    DK_PROFILE_ZONE("render.upload_disk_scene");
    if (!state_ || state_->heap.state()!=memory::ResourceState::open || queue.stats().closed || queue.stats().device_lost)
        return std::unexpected(Error{ErrorCode::invalid_state,"disk scene, memory or queue unavailable"});
    try {
        auto candidate=take(GpuAssets::create(state_->heap));
        for (const auto& data : state_->assets) {
            if(data->voxel) continue;
            auto asset=take(candidate.upload(queue,*data));
            require(take(asset.wait(queue)),"disk scene upload wait timed out",ErrorCode::invalid_state);
        }
        return candidate;
    } catch (Error& error) { return std::unexpected(error.with_context("DiskScene.upload")); }
      catch (const std::bad_alloc&) { return std::unexpected(Error{ErrorCode::internal_error,"disk scene upload allocation failed"}); }
}
std::size_t DiskScene::cpu_bytes() const noexcept { return state_ ? state_->cpu_bytes : 0; }
bool DiskScene::has_voxels() const noexcept {
    return state_ && std::ranges::any_of(state_->assets,[](const auto& a){return a->voxel.has_value();});
}
#ifdef DK_DISK_VOXELS
Result<VoxelAssets> DiskScene::upload_voxels(graphics::SubmissionQueue& queue) const {
    if(!state_ || state_->heap.state()!=memory::ResourceState::open || queue.stats().closed || queue.stats().device_lost)
        return std::unexpected(Error{ErrorCode::invalid_state,"disk scene, memory or queue unavailable"});
    try {
        auto candidate=take(VoxelAssets::create(state_->heap,queue));
        for(const auto& data:state_->assets) if(data->voxel) {
            auto asset=take(candidate.upload(queue,*data));
            require(take(asset.wait(queue)),"disk voxel upload wait timed out",ErrorCode::invalid_state);
        }
        return candidate;
    } catch(Error& error) { return std::unexpected(error.with_context("DiskScene.upload_voxels")); }
      catch(const std::bad_alloc&) { return std::unexpected(Error{ErrorCode::internal_error,"disk voxel upload allocation failed"}); }
}
#endif
} // namespace dk::render

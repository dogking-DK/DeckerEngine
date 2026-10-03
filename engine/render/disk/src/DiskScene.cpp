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

namespace dk::render {
namespace detail {
struct DiskSceneState {
    explicit DiskSceneState(memory::ResourceHandle resource)
        : heap(resource), assets(0,memory::Allocator<std::shared_ptr<const CpuAsset>>{resource}) {}
    memory::ResourceHandle heap;
    RenderScene scene;
    Vector<std::shared_ptr<const CpuAsset>> assets;
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
    return result;
}
CpuAsset load_asset(const Project& project,AssetReference reference,const DiskSceneOptions& options) {
    const auto path=take(project.resolve_asset(reference));
    const auto extension=path.extension();
    if (path.filename()=="manifest.json") {
        auto artifact=take(load_cpu_artifact(path.parent_path()));
        require(artifact.data.mesh.id==reference.id,"CPU artifact mesh ID differs from Project",ErrorCode::conflict);
        return std::move(artifact.data);
    }
    require(extension==".gltf" || extension==".glb","mesh path must be glTF/GLB or CPU artifact manifest.json",ErrorCode::not_supported);
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
        auto mesh=std::ranges::find(identities,std::string_view{"mesh/0"},[](const auto& output){return std::string_view{output.key};});
        require(mesh!=identities.end() && mesh->id==reference.id && mesh->kind==AssetKind::mesh,
            "source metadata mesh/0 ID differs from Project",ErrorCode::conflict);
    } else identities.push_back({String{"mesh/0"},reference.id,AssetKind::mesh});
    GltfImportRequest request{relative,identities,scale}; request.profile=options.profile; request.stop=options.stop;
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
    const DiskSceneOptions hard;
    if (!options.max_assets || options.max_assets>hard.max_assets || !options.cpu_bytes || options.cpu_bytes>hard.cpu_bytes ||
        (options.profile!=GltfImportProfile::strict && options.profile!=GltfImportProfile::unlit_preview))
        return std::unexpected(Error{ErrorCode::invalid_argument,"invalid disk scene limits/profile"});
    try {
        memory::DomainScope domain{heap};
        check(check_asset_operations(project.paths().root()));
        require(!options.stop.stop_requested(),"disk scene load cancelled",ErrorCode::invalid_state);
        auto candidate=memory::make_shared_in<detail::DiskSceneState>(heap,heap);
        candidate->scene=take(RenderScene::extract(heap,snapshot));
        std::size_t used=0;
        for (std::size_t i=0;i<candidate->scene.entities().size();++i) {
            for (auto reference : take(candidate->scene.assets(i))) {
                if (reference.kind!=AssetKind::mesh) continue;
                if (std::ranges::any_of(candidate->assets,[&](const auto& asset){return asset->mesh.id==reference.id;})) continue;
                require(candidate->assets.size()<options.max_assets,"disk scene asset count exceeds limit");
                require(!options.stop.stop_requested(),"disk scene load cancelled",ErrorCode::invalid_state);
                auto loaded=load_asset(project,reference,options);
                check(validate_gpu_asset(loaded));
                for (const auto& material : loaded.materials)
                    require(material.alpha_mode!=AlphaMode::blend,"disk scene alpha blend is not supported",ErrorCode::not_supported);
                used+=bytes(loaded,options.cpu_bytes-used);
                candidate->assets.push_back(memory::make_shared_in<CpuAsset>(heap,std::move(loaded)));
            }
        }
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
            auto asset=take(candidate.upload(queue,*data));
            require(take(asset.wait(queue)),"disk scene upload wait timed out",ErrorCode::invalid_state);
        }
        return candidate;
    } catch (Error& error) { return std::unexpected(error.with_context("DiskScene.upload")); }
      catch (const std::bad_alloc&) { return std::unexpected(Error{ErrorCode::internal_error,"disk scene upload allocation failed"}); }
}
} // namespace dk::render

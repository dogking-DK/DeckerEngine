#include <dk/render/VoxelAssets.hpp>
#include <dk/render/VoxelGpu.hpp>
#include <dk/memory/SmartPtr.hpp>
#include <dk/memory/Context.hpp>
#include <algorithm>
#include <cmath>
namespace dk::render {
namespace detail {
struct VoxelAssetState { AssetId id; std::uint64_t generation=0; std::shared_ptr<const VoxelGpuGrid> grid; Vector<VoxelMaterialData> palette; };
struct VoxelAssetSlot { AssetId id; std::unique_ptr<VoxelGpuCache> cache; std::shared_ptr<const VoxelAssetState> current; };
struct VoxelAssetCacheState {
    explicit VoxelAssetCacheState(memory::ResourceHandle h) : heap(h),slots(0,memory::Allocator<VoxelAssetSlot>{h}) {}
    memory::ResourceHandle heap; graphics::Buffer identity; VoxelAssetLimits limits;
    Vector<VoxelAssetSlot> slots; std::uint64_t generation=0;
};
}
namespace {
template<class T> T take(Result<T> r) { if(!r) throw r.error(); return std::move(*r); }
void require(bool b,const char* text) { if(!b) throw Error{ErrorCode::invalid_argument,text}; }
}
Result<VoxelAssets> VoxelAssets::create(memory::ResourceHandle heap,graphics::SubmissionQueue& q,VoxelAssetLimits limits) {
    try {
        require(heap && heap.state()==memory::ResourceState::open,"Voxel assets require open memory");
        require(limits.assets>0 && limits.assets<=64 && limits.grid_bytes>0 && limits.grid_bytes<=2ull*1024*1024*1024 && limits.generations>0 && limits.generations<=2,"Invalid voxel asset limits");
        auto state=memory::make_shared_in<detail::VoxelAssetCacheState>(heap,heap); state->limits=limits;
        state->identity=take(q.create_buffer({4})); return VoxelAssets{std::move(state)};
    } catch(const Error& e) { return std::unexpected(e); }
      catch(const std::bad_alloc&) { return std::unexpected(Error{ErrorCode::internal_error,"Voxel cache allocation failed"}); }
}
Result<GpuVoxelAsset> VoxelAssets::upload(graphics::SubmissionQueue& q,const CpuAsset& asset) {
    if(!state_ || state_->heap.state()!=memory::ResourceState::open || q.stats().closed || q.stats().device_lost)
        return std::unexpected(Error{ErrorCode::invalid_state,"Voxel cache, memory or queue unavailable"});
    try {
        auto& s=*state_; { auto guard=take(q.begin()); if(auto r=guard.retain(s.identity); !r) throw r.error(); }
        require(asset.voxel && asset.voxel->grid && !asset.voxel->id.is_nil() && asset.mesh.id.is_nil() && asset.mesh.primitives.empty() && asset.materials.empty() && asset.textures.empty(),"Expected exclusive voxel CPU payload");
        const auto& v=*asset.voxel;
        require(asset.outputs.size()==1 && asset.outputs[0].id==v.id && asset.outputs[0].kind==AssetKind::voxel && asset.outputs[0].key=="voxel/0","Voxel output identity mismatch");
        require(!v.palette.empty() && v.palette.size()<=65536 && v.palette.size()>=v.grid->maximum_value(),"Incomplete voxel palette");
        for(const auto& m:v.palette) require(m.base_color.allFinite() && (m.base_color.array()>=0).all() && (m.base_color.array()<=1).all() && std::isfinite(m.metallic) && m.metallic>=0 && m.metallic<=1 && std::isfinite(m.roughness) && m.roughness>=0 && m.roughness<=1,"Invalid voxel palette material");
        const auto found=std::ranges::find(s.slots,v.id,&detail::VoxelAssetSlot::id); const auto index=std::size_t(found-s.slots.begin());
        require(index<s.slots.size() || s.slots.size()<s.limits.assets,"Voxel asset count budget exceeded"); require(s.generation<UINT64_MAX,"Voxel asset generation exhausted");
        s.slots.reserve(s.slots.size()+(index==s.slots.size() ? 1 : 0));
        memory::DomainScope domain{s.heap}; auto candidate=memory::make_shared_in<detail::VoxelAssetState>(s.heap);
        candidate->id=v.id; candidate->generation=s.generation+1; candidate->palette=v.palette;
        auto fresh=index==s.slots.size() ? take(VoxelGpuCache::create(q,{s.limits.grid_bytes,s.limits.generations})) : std::unique_ptr<VoxelGpuCache>{};
        auto* cache=fresh ? fresh.get() : s.slots[index].cache.get();
        candidate->grid=take(cache->prepare(q,*v.grid)); // Commit: all subsequent operations are nonallocating.
        if(fresh) s.slots.push_back({v.id,std::move(fresh),candidate}); else s.slots[index].current=candidate;
        s.generation=candidate->generation; return GpuVoxelAsset{std::move(candidate)};
    } catch(const Error& e) { return std::unexpected(e); }
      catch(const std::bad_alloc&) { return std::unexpected(Error{ErrorCode::internal_error,"Voxel asset upload allocation failed; current unchanged"}); }
}
Result<GpuVoxelAsset> VoxelAssets::find(AssetId id) const {
    if(state_) for(const auto& slot:state_->slots) if(slot.id==id) return GpuVoxelAsset{slot.current};
    return std::unexpected(Error{ErrorCode::not_found,"Voxel GPU asset not loaded"});
}
bool VoxelAssets::unload(AssetId id) noexcept {
    if(!state_) return false; const auto old=state_->slots.size(); std::erase_if(state_->slots,[&](const auto& s){return s.id==id;}); return old!=state_->slots.size();
}
void VoxelAssets::clear() noexcept { if(state_) state_->slots.clear(); }
std::size_t VoxelAssets::size() const noexcept { return state_ ? state_->slots.size() : 0; }
AssetId GpuVoxelAsset::id() const noexcept { return state_ ? state_->id : AssetId{}; }
std::uint64_t GpuVoxelAsset::generation() const noexcept { return state_ ? state_->generation : 0; }
std::shared_ptr<const VoxelGpuGrid> GpuVoxelAsset::grid() const noexcept { return state_ ? state_->grid : nullptr; }
std::span<const VoxelMaterialData> GpuVoxelAsset::palette() const noexcept { return state_ ? std::span<const VoxelMaterialData>{state_->palette} : std::span<const VoxelMaterialData>{}; }
Result<bool> GpuVoxelAsset::wait(graphics::SubmissionQueue& q,std::uint64_t timeout) const {
    if(!state_) return std::unexpected(Error{ErrorCode::invalid_state,"Empty voxel GPU asset"}); return q.wait(state_->grid->upload_submission(),timeout);
}
} // namespace dk::render

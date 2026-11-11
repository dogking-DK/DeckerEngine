#pragma once
#include <dk/render/GpuAssets.hpp>
namespace dk::render {
class VoxelGpuGrid;
namespace detail { struct VoxelAssetState; struct VoxelAssetCacheState; }
struct VoxelAssetLimits { std::size_t assets=64; std::uint64_t grid_bytes=512ull*1024*1024; std::uint32_t generations=2; };
class GpuVoxelAsset {
public:
    GpuVoxelAsset()=default;
    [[nodiscard]] explicit operator bool() const noexcept { return bool(state_); }
    [[nodiscard]] AssetId id() const noexcept;
    [[nodiscard]] std::uint64_t generation() const noexcept;
    [[nodiscard]] std::shared_ptr<const VoxelGpuGrid> grid() const noexcept;
    [[nodiscard]] std::span<const VoxelMaterialData> palette() const noexcept;
    [[nodiscard]] Result<bool> wait(graphics::SubmissionQueue&,std::uint64_t timeout_ns=UINT64_MAX) const;
private:
    friend class VoxelAssets;
    explicit GpuVoxelAsset(std::shared_ptr<const detail::VoxelAssetState> state) : state_(std::move(state)) {}
    std::shared_ptr<const detail::VoxelAssetState> state_;
};
// Owner-thread, single queue. Replacement prepares complete immutable versions before publish.
// Limits are per asset cache (current + retained generations), not a process-wide GPU quota.
class VoxelAssets {
public:
    VoxelAssets()=default;
    VoxelAssets(VoxelAssets&&) noexcept=default;
    VoxelAssets& operator=(VoxelAssets&&) noexcept=default;
    VoxelAssets(const VoxelAssets&)=delete;
    VoxelAssets& operator=(const VoxelAssets&)=delete;
    [[nodiscard]] static Result<VoxelAssets> create(memory::ResourceHandle,graphics::SubmissionQueue&,VoxelAssetLimits = {});
    [[nodiscard]] Result<GpuVoxelAsset> upload(graphics::SubmissionQueue&,const CpuAsset&);
    [[nodiscard]] Result<GpuVoxelAsset> find(AssetId) const;
    [[nodiscard]] bool unload(AssetId) noexcept;
    void clear() noexcept;
    [[nodiscard]] std::size_t size() const noexcept;
private:
    explicit VoxelAssets(std::shared_ptr<detail::VoxelAssetCacheState> state) : state_(std::move(state)) {}
    std::shared_ptr<detail::VoxelAssetCacheState> state_;
};
} // namespace dk::render

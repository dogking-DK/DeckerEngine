#pragma once
#include <dk/assets/CpuData.hpp>
#include <dk/graphics/GpuObjects.hpp>

namespace dk::render {
namespace detail { struct GpuAssetState; struct GpuCacheState; }
struct GpuVertex { float position[3], normal[3], uv[2]; };
static_assert(sizeof(GpuVertex) == 32);
struct GpuPrimitive {
    graphics::Buffer vertices, indices;
    std::uint32_t vertex_count = 0, index_count = 0;
    Vec3f bounds_min = Vec3f::Zero(), bounds_max = Vec3f::Zero();
    std::optional<AssetId> material;
    bool has_normals = false, has_texcoords = false;
};
// Borrowed mesh view; retain the GpuAsset that supplied it.
struct GpuMesh { AssetId id; std::span<const GpuPrimitive> primitives; };
struct GpuTexture {
    AssetId id;
    graphics::Image image;
    graphics::ImageView view;
    graphics::Sampler sampler;
    graphics::SamplerDesc sampler_description;
};
// Pure CPU validation; does not initialize or require a GPU.
[[nodiscard]] Result<void> validate_gpu_asset(const CpuAsset&);
// An immutable version, independently retained across cache reload/unload/destruction.
// Publication means submitted. Use wait() before claiming host-observed completion.
class GpuAsset final {
public:
    GpuAsset() = default;
    [[nodiscard]] explicit operator bool() const noexcept { return bool(state_); }
    [[nodiscard]] AssetId id() const noexcept;
    [[nodiscard]] std::uint64_t generation() const noexcept;
    [[nodiscard]] graphics::Submission submission() const noexcept;
    [[nodiscard]] GpuMesh mesh() const noexcept;
    [[nodiscard]] std::span<const GpuTexture> textures() const noexcept;
    [[nodiscard]] std::span<const MaterialData> materials() const noexcept;
    [[nodiscard]] Result<bool> wait(graphics::SubmissionQueue&, std::uint64_t timeout_ns = std::numeric_limits<std::uint64_t>::max()) const;
private:
    friend class GpuAssets;
    explicit GpuAsset(std::shared_ptr<const detail::GpuAssetState> state) : state_(std::move(state)) {}
    std::shared_ptr<const detail::GpuAssetState> state_;
};
// Single owner thread; externally serialize cache and queue accesses.
class GpuAssets final {
public:
    GpuAssets() = default;
    GpuAssets(GpuAssets&&) noexcept = default;
    GpuAssets& operator=(GpuAssets&&) noexcept = default;
    GpuAssets(const GpuAssets&) = delete;
    GpuAssets& operator=(const GpuAssets&) = delete;
    [[nodiscard]] static Result<GpuAssets> create(memory::ResourceHandle);
    [[nodiscard]] Result<GpuAsset> upload(graphics::SubmissionQueue&, const CpuAsset&);
    [[nodiscard]] Result<GpuAsset> find(AssetId mesh) const;
    [[nodiscard]] bool unload(AssetId mesh) noexcept;
    void clear() noexcept;
    [[nodiscard]] std::size_t size() const noexcept;
private:
    explicit GpuAssets(std::shared_ptr<detail::GpuCacheState> state) : state_(std::move(state)) {}
    std::shared_ptr<detail::GpuCacheState> state_;
};
} // namespace dk::render

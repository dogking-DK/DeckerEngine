#pragma once
#include <dk/geometry/SparseVoxels.hpp>
#include <dk/graphics/Resources.hpp>
#include <filesystem>

namespace dk::render {
namespace detail { struct VoxelQueryState; struct VoxelFrameState; }
class VoxelGpuGrid {
public:
    [[nodiscard]] const graphics::Buffer& buffer() const noexcept { return buffer_; }
    [[nodiscard]] const geometry::voxels::SparseGrid& source() const noexcept { return source_; }
    [[nodiscard]] std::uint64_t generation() const noexcept { return generation_; }
    [[nodiscard]] graphics::Submission upload_submission() const noexcept { return upload_; }
private:
    friend class VoxelGpuCache;
    explicit VoxelGpuGrid(geometry::voxels::SparseGrid source) : source_(std::move(source)) {}
    geometry::voxels::SparseGrid source_;
    graphics::Buffer buffer_;
    graphics::Submission upload_;
    std::uint64_t generation_ = 0;
};
struct VoxelGpuLimits {
    std::uint64_t bytes = 512ull * 1024 * 1024; // Live grids + candidate + upload staging payload.
    std::uint32_t generations = 2;
};
class VoxelGpuCache {
public:
    [[nodiscard]] static Result<std::unique_ptr<VoxelGpuCache>> create(graphics::SubmissionQueue&, VoxelGpuLimits = {});
    [[nodiscard]] Result<std::shared_ptr<const VoxelGpuGrid>> prepare(graphics::SubmissionQueue&, const geometry::voxels::SparseGrid&);
    [[nodiscard]] std::shared_ptr<const VoxelGpuGrid> current() const noexcept { return current_; }
private:
    VoxelGpuCache(graphics::Buffer identity, VoxelGpuLimits limits) : identity_(std::move(identity)), limits_(limits) {}
    graphics::Buffer identity_;
    VoxelGpuLimits limits_;
    std::vector<std::shared_ptr<const VoxelGpuGrid>> live_;
    std::shared_ptr<const VoxelGpuGrid> current_;
};
enum class VoxelGpuStatus : std::uint32_t { miss, hit, sample, work_limit, numeric_error, format_error, address_error };
// Fixed 64-byte GPU output. Distance is the original ray parameter, normal is world space.
struct VoxelGpuResult {
    VoxelGpuStatus status{};
    std::uint32_t value = 0, face = 0, reserved = 0;
    geometry::voxels::Coord cell{};
    std::int32_t padding = 0;
    std::array<float,3> normal{};
    float distance = 0;
    std::uint32_t visited_regions = 0, skipped_regions = 0, dimension = 0, active = 0;
};
struct VoxelQueryLimits {
    std::uint64_t bytes = 256ull * 1024 * 1024; // Input/uniform + output + readback payload, per query.
    std::uint32_t maximum_regions = 1024 * 1024;
};
// Owns asynchronous work and readback. Timeout leaves the caller's output unchanged.
class VoxelGpuFrame {
public:
    VoxelGpuFrame() = default;
    [[nodiscard]] graphics::Submission submission() const;
    [[nodiscard]] Result<bool> read(graphics::SubmissionQueue&, std::vector<VoxelGpuResult>& output, std::uint64_t timeout_ns = UINT64_MAX) const;
private:
    friend class VoxelGpuQuery;
    explicit VoxelGpuFrame(std::shared_ptr<detail::VoxelFrameState> state) : state_(std::move(state)) {}
    std::shared_ptr<detail::VoxelFrameState> state_;
};
class VoxelGpuQuery {
public:
    [[nodiscard]] static Result<VoxelGpuQuery> create(memory::ResourceHandle, graphics::SubmissionQueue&,
        const std::filesystem::path& shader_directory = {}, const std::filesystem::path& nano_include_directory = {});
    [[nodiscard]] Result<VoxelGpuFrame> trace(graphics::SubmissionQueue&, std::shared_ptr<const VoxelGpuGrid>,
        std::span<const geometry::Ray>, const Transformd& local_to_world = {}, VoxelQueryLimits = {}) const;
    [[nodiscard]] Result<VoxelGpuFrame> sample(graphics::SubmissionQueue&, std::shared_ptr<const VoxelGpuGrid>,
        std::span<const geometry::voxels::Coord>, VoxelQueryLimits = {}) const;
    [[nodiscard]] std::string_view shader_reflection() const;
private:
    explicit VoxelGpuQuery(std::shared_ptr<detail::VoxelQueryState> state) : state_(std::move(state)) {}
    std::shared_ptr<detail::VoxelQueryState> state_;
};
} // namespace dk::render

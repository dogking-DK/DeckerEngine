#pragma once
#include <dk/render/PathTraceReference.hpp>
#include <dk/graphics/Resources.hpp>
#include <filesystem>

namespace dk::render {
namespace detail { struct PathGpuState; struct PathGpuScene; struct PathGpuFrame; struct PathGpuImage; }
// Immutable queue-domain scene; CPU source owns provenance and complete world geometry.
class PathTraceGpuScene final {
public:
    PathTraceGpuScene() = default;
    [[nodiscard]] explicit operator bool() const noexcept { return bool(state_); }
    [[nodiscard]] graphics::Submission submission() const noexcept; // Empty scene needs no BUILD submission.
    [[nodiscard]] const PathTraceScene& source() const noexcept;
private:
    friend class PathTraceRenderer;
    explicit PathTraceGpuScene(std::shared_ptr<const detail::PathGpuScene> state) : state_(std::move(state)) {}
    std::shared_ptr<const detail::PathGpuScene> state_;
};
struct PathTraceGpuImageInfo : PathTraceImageInfo {
    bool fixed_rays = false;
    std::uint32_t first_pixel = 0; // Counter origin for fixed-ray batches.
    // Hardware traversal counts are unavailable; statistics.triangle_tests stays zero.
};
class PathTraceGpuImage final {
public:
    PathTraceGpuImage() = default;
    [[nodiscard]] explicit operator bool() const noexcept { return bool(state_); }
    [[nodiscard]] std::span<const Vec3d> linear_rgb() const noexcept;
    [[nodiscard]] std::span<const Vec3d> second_moment() const noexcept;
    // First sample's primary hit per pixel; indices refer to the prepared scene's CPU source.
    [[nodiscard]] std::span<const std::optional<PathTraceHit>> primary_hits() const noexcept;
    [[nodiscard]] const PathTraceGpuImageInfo& info() const noexcept;
private:
    friend class PathTraceFrame;
    explicit PathTraceGpuImage(std::shared_ptr<const detail::PathGpuImage> state) : state_(std::move(state)) {}
    std::shared_ptr<const detail::PathGpuImage> state_;
};
class PathTraceFrame final {
public:
    PathTraceFrame() = default;
    [[nodiscard]] explicit operator bool() const noexcept { return bool(state_); }
    [[nodiscard]] graphics::Submission submission() const noexcept;
    // No blocking wait. Empty optional means pending; errors/cancellation publish no partial image.
    [[nodiscard]] Result<std::optional<PathTraceGpuImage>> read(graphics::SubmissionQueue&, std::stop_token = {}) const;
private:
    friend class PathTraceRenderer;
    explicit PathTraceFrame(std::shared_ptr<const detail::PathGpuFrame> state) : state_(std::move(state)) {}
    std::shared_ptr<const detail::PathGpuFrame> state_;
};
// One queue, externally serialized. No history, implicit waits or mutable completed-frame slot.
class PathTraceRenderer final {
public:
    PathTraceRenderer() = default;
    [[nodiscard]] static Result<PathTraceRenderer> create(memory::ResourceHandle, graphics::SubmissionQueue&,
        const std::filesystem::path& shader_directory, std::stop_token = {});
    [[nodiscard]] Result<PathTraceGpuScene> prepare_scene(graphics::SubmissionQueue&, const PathTraceScene&, std::stop_token = {}) const;
    [[nodiscard]] Result<PathTraceFrame> submit_view(graphics::SubmissionQueue&, const PathTraceGpuScene&, const RenderView&,
        const PathTraceSettings& = {}, PathTraceSamples = {}, std::stop_token = {}) const;
    [[nodiscard]] Result<PathTraceFrame> submit_rays(graphics::SubmissionQueue&, const PathTraceGpuScene&, std::span<const PathTraceRay>,
        const PathTraceSettings& = {}, PathTraceSamples = {}, std::uint32_t first_pixel = 0, std::stop_token = {}) const;
private:
    explicit PathTraceRenderer(std::shared_ptr<detail::PathGpuState> state) : state_(std::move(state)) {}
    std::shared_ptr<detail::PathGpuState> state_;
};
} // namespace dk::render

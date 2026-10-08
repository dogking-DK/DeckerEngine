#pragma once
#include <dk/render/RenderScene.hpp>
#include <dk/render/GpuAssets.hpp>
#include <filesystem>

namespace dk::render {
namespace detail { struct PipelineState; struct FrameState; }
struct RenderSettings {
    Vec3f clear_rgb = Vec3f::Zero(); // Linear HDR color.
    float exposure = 1;
    bool capture_plan = false;
};
// Shared by the editor viewport and Runtime capture for reproducible unlit previews.
[[nodiscard]] inline RenderSettings unlit_preview_settings() { return {{0.04f,0.08f,0.16f},1,false}; }
[[nodiscard]] Result<void> validate_render_settings(const RenderView&, const RenderSettings&);
struct FrameInfo {
    SceneId scene;
    std::uint64_t revision = 0, frame = 0;
    std::uint32_t width = 0, height = 0;
    std::size_t draw_count = 0;
};
// Shared immutable result metadata; GPU completion is explicit. Views borrow this owner.
class RenderFrame final {
public:
    RenderFrame() = default;
    [[nodiscard]] explicit operator bool() const noexcept { return bool(state_); }
    [[nodiscard]] FrameInfo info() const noexcept;
    [[nodiscard]] graphics::Submission submission() const noexcept;
    [[nodiscard]] Result<bool> wait(graphics::SubmissionQueue&, std::uint64_t timeout_ns = std::numeric_limits<std::uint64_t>::max()) const;
    [[nodiscard]] Result<void> read_rgba8(std::span<std::byte> destination) const;
    [[nodiscard]] Result<const graphics::Image*> color() const;
    [[nodiscard]] std::string_view plan_text() const noexcept;
private:
    friend class ScenePipeline;
    explicit RenderFrame(std::shared_ptr<const detail::FrameState> state) : state_(std::move(state)) {}
    std::shared_ptr<const detail::FrameState> state_;
};
// Single queue, externally serialized. Does not own or store a raw queue pointer.
class ScenePipeline final {
public:
    ScenePipeline() = default;
    ScenePipeline(ScenePipeline&&) noexcept = default;
    ScenePipeline& operator=(ScenePipeline&&) noexcept = default;
    ScenePipeline(const ScenePipeline&) = delete;
    ScenePipeline& operator=(const ScenePipeline&) = delete;
    [[nodiscard]] static Result<ScenePipeline> create(memory::ResourceHandle, graphics::SubmissionQueue&,
        const std::filesystem::path& shader_directory);
    [[nodiscard]] Result<RenderFrame> render(graphics::SubmissionQueue&, const RenderView&, const GpuAssets&,
        const RenderSettings& = {});
private:
    explicit ScenePipeline(std::shared_ptr<detail::PipelineState> state) : state_(std::move(state)) {}
    std::shared_ptr<detail::PipelineState> state_;
};
} // namespace dk::render

#pragma once
#include <dk/physics/GpuXpbd.hpp>

namespace dk::render {
namespace detail { struct ClothRendererState; }
struct ClothView {
    std::uint32_t columns = 8, rows = 8, width = 640, height = 480;
    // Row-major world -> Vulkan clip coordinates; default fits the default XPBD cloth.
    std::array<float,16> view_projection{1.2f,0,0.4f,-0.2f, 0,-0.9f,0.65f,0.15f, 0,-0.325f,-0.45f,0.8f, 0,0,0,1};
    float floor_y = 0;
    bool image_readback = false;
};
class SimulationFrame final {
public:
    [[nodiscard]] const GpuXpbdFrame& physics() const noexcept { return physics_; }
    [[nodiscard]] Result<const graphics::Image*> color() const;
    [[nodiscard]] Result<void> read_rgba8(std::span<std::byte>) const;
    [[nodiscard]] std::uint32_t width() const noexcept { return width_; }
    [[nodiscard]] std::uint32_t height() const noexcept { return height_; }
private:
    friend class ClothRenderer;
    GpuXpbdFrame physics_;
    std::size_t color_ = 0, readback_ = 0;
    std::uint32_t width_ = 0, height_ = 0;
    bool has_readback_ = false;
};
class ClothRenderer final {
public:
    ClothRenderer() = default;
    [[nodiscard]] static Result<ClothRenderer> create(memory::ResourceHandle, graphics::SubmissionQueue&,
        const std::filesystem::path& shader_file);
    // One Graph/one submission for candidate simulation, visualization and optional diagnostics.
    [[nodiscard]] Result<SimulationFrame> render(graphics::SubmissionQueue&, GpuXpbdSolver&, std::int64_t dt_ns,
        std::uint32_t count, const ClothView& = {}, GpuXpbdOptions = {});
private:
    explicit ClothRenderer(std::shared_ptr<detail::ClothRendererState> state) : state_(std::move(state)) {}
    std::shared_ptr<detail::ClothRendererState> state_;
};
}

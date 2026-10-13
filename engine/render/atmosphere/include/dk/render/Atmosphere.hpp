#pragma once
#include <dk/render/AtmosphereReference.hpp>
#include <dk/graphics/GraphExecution.hpp>
#include <filesystem>

namespace dk::render {
namespace detail { struct AtmosphereState; struct SkyState; struct SkyFrameState; }
enum class AtmosphereLut { transmittance, multiple_scattering, sky_view };
struct AtmosphereGenerations {
    std::uint64_t transmittance=0, multiple_scattering=0, sky_view=0;
    bool operator==(const AtmosphereGenerations&) const = default;
};
// Borrowed immutable CPU mirror: linear RGB, alpha = max feedback for MS, otherwise 1.
struct AtmosphereLutView { std::uint32_t width=0,height=0; std::span<const std::array<float,4>> texels; };
class AtmosphereSky final {
public:
    AtmosphereSky() = default;
    [[nodiscard]] explicit operator bool() const noexcept { return bool(state_); }
    [[nodiscard]] AtmosphereGenerations generations() const noexcept;
    [[nodiscard]] Result<AtmosphereLutView> lut(AtmosphereLut) const;
    [[nodiscard]] Result<EnvironmentDescription> environment() const;
    [[nodiscard]] Result<Vec3d> position_m() const;
    // Borrowed immutable GPU LUT; retain this snapshot until recording has completed.
    [[nodiscard]] Result<const graphics::Image*> image(AtmosphereLut) const;
private:
    friend class AtmosphereRenderer;
    explicit AtmosphereSky(std::shared_ptr<const detail::SkyState> state) : state_(std::move(state)) {}
    std::shared_ptr<const detail::SkyState> state_;
};
struct SkyCamera {
    Vec3d forward{0,0,-1}, up{0,1,0};
    double vertical_fov_rad=1.0471975511965976;
    std::uint32_t width=256,height=128;
    bool sun_disk=true;
    bool reference=false; // Direct 512 x 512 integration, with the same optional MS closure.
};
class SkyFrame final {
public:
    SkyFrame() = default;
    [[nodiscard]] explicit operator bool() const noexcept { return bool(state_); }
    [[nodiscard]] graphics::Submission submission() const noexcept;
    [[nodiscard]] Result<bool> wait(graphics::SubmissionQueue&,std::uint64_t timeout_ns=std::numeric_limits<std::uint64_t>::max()) const;
    [[nodiscard]] Result<void> read_hdr(std::span<std::array<float,4>>) const;
    [[nodiscard]] Result<const graphics::Image*> color() const;
private:
    friend class AtmosphereRenderer;
    explicit SkyFrame(std::shared_ptr<const detail::SkyFrameState> state) : state_(std::move(state)) {}
    std::shared_ptr<const detail::SkyFrameState> state_;
};
// One queue, externally serialized. prepare waits and validates before publishing;
// render_sky is asynchronous. Old immutable snapshots/frames keep their own resources.
class AtmosphereRenderer final {
public:
    AtmosphereRenderer() = default;
    AtmosphereRenderer(AtmosphereRenderer&&) noexcept = default;
    AtmosphereRenderer& operator=(AtmosphereRenderer&&) noexcept = default;
    AtmosphereRenderer(const AtmosphereRenderer&) = delete;
    AtmosphereRenderer& operator=(const AtmosphereRenderer&) = delete;
    [[nodiscard]] static Result<AtmosphereRenderer> create(memory::ResourceHandle,graphics::SubmissionQueue&,const std::filesystem::path& shader_directory);
    [[nodiscard]] Result<AtmosphereSky> prepare(graphics::SubmissionQueue&,const EnvironmentDescription&,const Vec3d& position_m,bool multiple_scattering=true);
    [[nodiscard]] Result<SkyFrame> render_sky(graphics::SubmissionQueue&,const AtmosphereSky&,const SkyCamera& = {});
    [[nodiscard]] AtmosphereGenerations generations() const noexcept;
private:
    explicit AtmosphereRenderer(std::shared_ptr<detail::AtmosphereState> state) : state_(std::move(state)) {}
    std::shared_ptr<detail::AtmosphereState> state_;
};
} // namespace dk::render

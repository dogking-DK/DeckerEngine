#pragma once
#include <dk/math/Types.hpp>
#include <array>
#include <bit>
#include <cstdint>
#include <cstddef>
#include <optional>
#include <memory>

namespace dk::render {
namespace detail { struct SignalReferenceAccess; }
// Versioned GPU storage layout, also readable without Graphics or a denoiser SDK.
// Sums are explicit, never reconstructed from differences of accumulated means.
struct DenoisingSignalPixel {
    using Float4 = std::array<float,4>;
    Float4 fresh_sum{}, fresh_second_sum{}, diffuse_sum{}, specular_sum{}, emission_sum{}, background_sum{};
    Float4 albedo_sum{}, normal_sum{}; // w: uint32 total guide samples / surface hits (bit pattern).
    Float4 normal_roughness{}, position_view_depth{}, albedo_metallic{};
    Float4 motion_distance{}; // xy: curr->prev pixels; z: continuation distance; w: primary ray distance.
    // Triangle index (UINT_MAX for voxel/miss), representative random sample, validity flags, fresh count.
    std::array<std::uint32_t,4> identity{};
    std::array<Float4,8> compensation{}; // Six fresh sums, accumulated albedo/normal; producer-owned.
    // v2: kind (0 miss, 1 triangle, 2 voxel), frozen triangle/instance index, face, palette value.
    std::array<std::uint32_t,4> surface{};
    std::array<std::int32_t,4> cell{}; // Signed voxel coordinates; w reserved.
    static constexpr std::uint32_t surface_valid=1, motion_valid=2, continuation_traced=4, continuation_hit=8;
    [[nodiscard]] std::uint32_t guide_samples() const noexcept { return std::bit_cast<std::uint32_t>(albedo_sum[3]); }
    [[nodiscard]] std::uint32_t guide_hits() const noexcept { return std::bit_cast<std::uint32_t>(normal_sum[3]); }
};
static_assert(sizeof(DenoisingSignalPixel)==23*16);
static_assert(offsetof(DenoisingSignalPixel,surface)==21*16 && offsetof(DenoisingSignalPixel,cell)==22*16);
struct DenoisingSignalIdentity {
    std::uint32_t version=2;
    std::uint64_t renderer_id=0,view_id=0,session=0,generation=0,source_serial=0;
    std::uint64_t fingerprint=0,geometry_fingerprint=0;
    std::uint32_t width=0,height=0,sample_first=0,sample_count=0,accumulated_spp=0,sampling_first_epoch=0;
    std::uint64_t content_version=0;
};
struct DenoisingMotionReference {
    DenoisingSignalIdentity source;
    Mat4d world_to_clip=Mat4d::Identity();
private:
    friend struct detail::SignalReferenceAccess;
    std::shared_ptr<const void> geometry_owner_;
};
struct PathTraceSignalOptions {
    bool enabled=false;
    // Caller selects the last actually consumed complete input, not the last UI redraw.
    std::optional<DenoisingMotionReference> previous;
};
struct DenoisingSignalInfo {
    DenoisingSignalIdentity source;
    std::optional<DenoisingMotionReference> previous;
};
} // namespace dk::render

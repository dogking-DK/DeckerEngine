#pragma once
#include <dk/render/CloudReference.hpp>

namespace dk::render {
struct CloudShadowSettings {
    Vec3d center_m=Vec3d::Zero(); // Fixed world region; never follows a camera implicitly.
    double extent_m=8192, time_seconds=0;
    std::uint32_t resolution=256, steps=256;
    bool homogeneous=false, diagnostics=false;
    double homogeneous_density=1;
    std::uint64_t max_work=128ULL*1024*1024;
    std::size_t max_bytes=64ULL*1024*1024;
    bool operator==(const CloudShadowSettings&) const = default;
};
struct CloudShadowProjection { Vec3d center_m,u,v,sun; double extent_m=0; };
[[nodiscard]] Result<CloudShadowProjection> cloud_shadow_projection(const EnvironmentDescription&,const CloudShadowSettings&);
[[nodiscard]] Result<CloudRay> cloud_shadow_ray(const EnvironmentDescription&,const CloudShadowSettings&,std::uint32_t x,std::uint32_t y);
[[nodiscard]] Result<Vec2d> cloud_shadow_coordinates(const EnvironmentDescription&,const CloudShadowSettings&,const Vec3d& receiver_m);
[[nodiscard]] std::size_t cloud_shadow_bytes(const CloudShadowSettings&) noexcept;
} // namespace dk::render

#include "CloudShadowInternal.hpp"
#include <bit>
#include <algorithm>
#include <cmath>

namespace dk::render {
std::size_t cloud_shadow_bytes(const CloudShadowSettings& s) noexcept {
    return std::size_t{s.resolution}*s.resolution*(128+4+(s.diagnostics?4:0))+208;
}
Result<CloudShadowProjection> cloud_shadow_projection(const EnvironmentDescription& input,const CloudShadowSettings& s) {
    auto normalized=normalize_environment(input); if(!normalized) return std::unexpected(normalized.error());
    const auto& e=*normalized; const auto& a=e.atmosphere; const auto& c=e.clouds;
    if(!s.center_m.allFinite() || (s.center_m.array().abs()>1e9).any() || !std::isfinite(s.extent_m) || s.extent_m<16 || s.extent_m>65536 ||
        !std::isfinite(s.time_seconds) || std::abs(s.time_seconds)>1e9 || !std::has_single_bit(s.resolution) || s.resolution<16 || s.resolution>512 ||
        !s.steps || s.steps>1024 || !std::isfinite(s.homogeneous_density) || s.homogeneous_density<0 || s.homogeneous_density>1 ||
        !s.max_work || s.max_work>128ULL*1024*1024 || !s.max_bytes || s.max_bytes>64ULL*1024*1024)
        return std::unexpected(Error{ErrorCode::invalid_argument,"Invalid cloud shadow region, sampling or budget"});
    if(a.ground_radius_m<1000 || a.ground_radius_m>1e7 || a.atmosphere_height_m>1e6 || c.top_height_m-c.bottom_height_m<1 ||
        c.weather_scale_m<100 || c.shape_scale_m<100 || c.detail_scale_m<10 ||
        std::ranges::any_of(a.planet_center_m,[](double v){ return std::abs(v)>1e8; }))
        return std::unexpected(Error{ErrorCode::not_supported,"Cloud shadow geometry or noise scale outside GPU domain"});
    if(std::uint64_t{s.resolution}*s.resolution*s.steps>s.max_work || cloud_shadow_bytes(s)>s.max_bytes)
        return std::unexpected(Error{ErrorCode::invalid_state,"Cloud shadow work or byte budget exceeded"});
    const Vec3d sun{e.sun.direction.data()},u=sun.unitOrthogonal();
    return CloudShadowProjection{s.center_m,u,sun.cross(u),sun,s.extent_m};
}
Result<CloudRay> cloud_shadow_ray(const EnvironmentDescription& input,const CloudShadowSettings& s,std::uint32_t x,std::uint32_t y) {
    auto p=cloud_shadow_projection(input,s); if(!p) return std::unexpected(p.error());
    if(x>=s.resolution || y>=s.resolution) return std::unexpected(Error{ErrorCode::invalid_argument,"Cloud shadow texel outside image"});
    return detail::shadow_ray(input,s,*p,x,y);
}
Result<Vec2d> cloud_shadow_coordinates(const EnvironmentDescription& e,const CloudShadowSettings& s,const Vec3d& receiver) {
    auto p=cloud_shadow_projection(e,s); if(!p) return std::unexpected(p.error());
    const Vec3d radial=receiver-Vec3d{e.atmosphere.planet_center_m.data()},delta=receiver-s.center_m;
    const double radius=radial.stableNorm(),inner=e.atmosphere.ground_radius_m+e.clouds.bottom_height_m;
    const Vec2d uv{.5+delta.dot(p->u)/s.extent_m,.5+delta.dot(p->v)/s.extent_m};
    const double edge=.5/s.resolution;
    if(!receiver.allFinite() || (receiver.array().abs()>1e9).any() || radius<e.atmosphere.ground_radius_m || radius>inner ||
        (uv.array()<edge).any() || (uv.array()>1-edge).any())
        return std::unexpected(Error{ErrorCode::not_supported,"Cloud shadow receiver outside below-cloud covered region"});
    return uv;
}
} // namespace dk::render

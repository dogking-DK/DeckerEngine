#pragma once
#include <dk/render/CloudShadow.hpp>

namespace dk::render::detail {
// All inputs have already passed cloud_shadow_projection. Double geometry and
// phase reduction keep world translations and long wind times out of float math.
inline CloudRay shadow_ray(const EnvironmentDescription& e,const CloudShadowSettings& s,
    const CloudShadowProjection& p,std::uint32_t x,std::uint32_t y) {
    const Vec3d center{e.atmosphere.planet_center_m.data()};
    const Vec3d point=p.center_m-center+p.u*((double(x)+.5)/s.resolution-.5)*s.extent_m+
        p.v*((double(y)+.5)/s.resolution-.5)*s.extent_m;
    const Vec3d perpendicular=point-p.sun*point.dot(p.sun);
    const double inner=e.atmosphere.ground_radius_m+e.clouds.bottom_height_m;
    const double outer=e.atmosphere.ground_radius_m+e.clouds.top_height_m;
    const double squared=perpendicular.squaredNorm();
    if(squared>=inner*inner) return {center+p.sun*inner,p.sun,0};
    const double a=std::sqrt(inner*inner-squared),b=std::sqrt(outer*outer-squared);
    return {center+perpendicular+p.sun*a,p.sun,(outer-inner)*(outer+inner)/(a+b)};
}
} // namespace dk::render::detail

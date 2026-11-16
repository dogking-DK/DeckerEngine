#pragma once
#include <dk/math/Types.hpp>
#include <algorithm>
#include <cmath>
#include <numbers>

namespace dk::editor::detail {
struct SunAngles { double azimuth=0, elevation=0; };
inline Vec3d sun_direction(SunAngles angles) {
    const auto a=angles.azimuth*std::numbers::pi/180, e=angles.elevation*std::numbers::pi/180;
    return {std::cos(e)*std::sin(a),std::sin(e),std::cos(e)*std::cos(a)};
}
inline SunAngles sun_angles(const Vec3d& direction) {
    if(!direction.allFinite() || direction.norm()==0) return {};
    const auto d=direction.normalized();
    return {std::atan2(d.x(),d.z())*180/std::numbers::pi,std::asin(std::clamp(d.y(),-1.,1.))*180/std::numbers::pi};
}
// Unit disk: +Z at the top, +X at the right; center zenith, half-radius horizon.
inline Vec2d sun_disk(SunAngles angles) {
    const auto radius=(90-angles.elevation)/180, a=angles.azimuth*std::numbers::pi/180;
    return {radius*std::sin(a),-radius*std::cos(a)};
}
inline SunAngles sun_from_disk(Vec2d point,double prior_azimuth=0) {
    const auto length=point.norm();
    return {length>1e-12 ? std::atan2(point.x(),-point.y())*180/std::numbers::pi : prior_azimuth,
        90-180*std::min(length,1.)};
}
}

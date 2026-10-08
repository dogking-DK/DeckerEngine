#include <dk/editor/Camera.hpp>
#include <algorithm>
#include <cmath>
#include <numbers>

namespace dk::editor {
namespace { constexpr double tangent=0.6370702608074932; } // tan(65 deg / 2)
void Camera::reset(bool fixture) {
    const Vec3d eye=fixture ? Vec3d{0,0,-2} : Vec3d{8,1.8,0};
    target_=fixture ? Vec3d{0,0,0} : Vec3d{-4,2,0};
    const Vec3d offset=eye-target_; distance_=offset.norm();
    yaw_=std::atan2(offset.x(),offset.z()); pitch_=std::asin(offset.y()/distance_); ++revision_;
}
Transformd Camera::world() const {
    const Vec3d backward{std::cos(pitch_)*std::sin(yaw_),std::sin(pitch_),std::cos(pitch_)*std::cos(yaw_)};
    const Vec3d right=Vec3d::UnitY().cross(backward).normalized(),up=backward.cross(right);
    Mat4d m=Mat4d::Identity();
    m.block<3,1>(0,0)=right; m.block<3,1>(0,1)=up; m.block<3,1>(0,2)=backward;
    m.block<3,1>(0,3)=target_+backward*distance_;
    return Transformd::from_matrix(m).value();
}
Mat4d Camera::projection(double aspect) const {
    constexpr double near=0.05,far=10000;
    Mat4d m=Mat4d::Zero(); m(0,0)=1/(tangent*aspect); m(1,1)=-1/tangent;
    m(2,2)=far/(near-far); m(2,3)=far*near/(near-far); m(3,2)=-1; return m;
}
Mat4d Camera::clip(double aspect) const { return projection(aspect)*world().inverse().value().matrix(); }
Result<geometry::Ray> Camera::ray(const Vec2d& uv,double aspect) const {
    if (!uv.allFinite() || (uv.array()<0).any() || (uv.array()>1).any() || !std::isfinite(aspect) || aspect<=0)
        return std::unexpected(Error{ErrorCode::invalid_argument,"Camera ray requires UV in [0,1] and positive aspect"});
    const auto transform=world();
    const Vec3d local{(2*uv.x()-1)*tangent*aspect,-(2*uv.y()-1)*tangent,-1};
    const Vec3d direction=transform.transform_direction(local.normalized()).value();
    // Ray interval matches near/far clipping planes, not spherical distance shells.
    return geometry::Ray{transform.matrix().block<3,1>(0,3),direction,0.05*local.norm(),10000*local.norm()};
}
std::optional<Vec2d> Camera::project(const Vec3d& point,double aspect) const {
    const Vec4d h=clip(aspect)*Vec4d{point.x(),point.y(),point.z(),1};
    if (!h.allFinite() || h.w()<=0 || h.z()<0 || h.z()>h.w()) return {};
    return Vec2d{(h.x()/h.w()+1)/2,(h.y()/h.w()+1)/2};
}
double Camera::units_per_pixel(const Vec3d& point,double height) const {
    const auto m=world().matrix();
    return 2*tangent*std::max(0.05,(m.block<3,1>(0,3)-point).dot(m.block<3,1>(0,2)))/std::max(1.0,height);
}
void Camera::orbit(double dx,double dy) {
    if (!std::isfinite(dx) || !std::isfinite(dy) || (dx==0 && dy==0)) return;
    yaw_=std::remainder(yaw_-dx*0.006,2*std::numbers::pi); pitch_=std::clamp(pitch_+dy*0.006,-1.55,1.55); ++revision_;
}
void Camera::pan(double dx,double dy,double height) {
    if (!std::isfinite(dx) || !std::isfinite(dy) || !std::isfinite(height) || height<=0 || (dx==0 && dy==0)) return;
    const auto m=world().matrix();
    const Vec3d candidate=target_+(-dx*m.block<3,1>(0,0)+dy*m.block<3,1>(0,1))*(2*tangent*distance_/height);
    if (!candidate.allFinite()) return;
    target_=candidate; ++revision_;
}
void Camera::dolly(double steps) {
    if (!std::isfinite(steps) || steps==0) return;
    distance_=std::clamp(distance_*std::exp(std::clamp(-steps*0.15,-20.0,20.0)),0.1,3000.0); ++revision_;
}
void Camera::frame(const geometry::Bounds& bounds,double aspect) {
    if (bounds.empty() || !bounds.minimum.allFinite() || !bounds.maximum.allFinite() || !std::isfinite(aspect) || aspect<=0) return;
    target_=bounds.minimum/2+bounds.maximum/2;
    const double radius=(bounds.maximum-bounds.minimum).norm()/2;
    distance_=std::clamp(radius*1.2/std::sin(std::atan(tangent*std::min(1.0,aspect))),0.1,3000.0); ++revision_;
}
}

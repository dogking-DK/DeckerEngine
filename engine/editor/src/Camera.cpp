#include <dk/editor/Camera.hpp>
#include <algorithm>
#include <cmath>
#include <numbers>

namespace dk::editor {
namespace {
bool range(double value,double minimum,double maximum) { return std::isfinite(value) && value>=minimum && value<=maximum; }
Error invalid(const char* message) { return {ErrorCode::invalid_argument,message}; }
}
void Camera::reset(bool fixture) {
    const Vec3d eye=fixture ? Vec3d{0,0,-2} : Vec3d{8,1.8,0};
    const Vec3d target=fixture ? Vec3d{0,0,0} : Vec3d{-4,2,0},offset=eye-target;
    const double distance=offset.norm(),yaw=std::atan2(offset.x(),offset.z()),pitch=std::asin(offset.y()/distance);
    if(eye_==eye && yaw_==yaw && pitch_==pitch && distance_==distance) return;
    eye_=eye; yaw_=yaw; pitch_=pitch; distance_=distance; ++revision_;
}
void Camera::set_environment(bool enabled) {
    const double before=near_plane(); environment_=enabled;
    if(near_plane()!=before) ++revision_;
}
Result<void> Camera::validate(double aspect) const {
    auto p=perspective_projection(lens(),aspect);
    if(!p) return std::unexpected(p.error());
    if(environment_ && (far_plane()>1e6 || far_plane()/near_plane()>10000 || (*p)(0,0)<.1 || -(*p)(1,1)<.1))
        return std::unexpected(invalid("Raster atmosphere/clouds require far <= 1e6, far/near <= 10000 and projection scales >= 0.1. Adjust FOV/clipping or use Auto."));
    return {};
}
Result<void> Camera::set_settings(const CameraSettings& value,double aspect) {
    const auto& l=value.lens; const auto& n=value.navigation;
    if(!std::isfinite(l.fov_y) || l.fov_y<=1 || l.fov_y>=179 || !std::isfinite(l.near_plane) ||
       !std::isfinite(l.far_plane) || l.near_plane<=0 || l.far_plane<=l.near_plane)
        return std::unexpected(invalid("Camera requires 1 < vertical FOV < 179 degrees and finite 0 < near < far"));
    if(!range(n.move_speed,.01,1000) || !range(n.fast_multiplier,1,100) || !range(n.slow_multiplier,.01,1) ||
       !range(n.look_sensitivity,.0001,.05) || !range(n.orbit_sensitivity,.0001,.05) ||
       !range(n.pan_multiplier,.01,10) || !range(n.wheel_sensitivity,.01,1))
        return std::unexpected(invalid("Camera navigation setting is outside its supported range"));
    Camera candidate=*this; candidate.settings_=value;
    if(auto valid=candidate.validate(aspect); !valid) return valid;
    const bool changed=fov_y()!=candidate.fov_y() || near_plane()!=candidate.near_plane() || far_plane()!=candidate.far_plane();
    settings_=value; if(changed) ++revision_; return {};
}
Result<void> Camera::restore_defaults(bool fixture,double aspect) {
    Camera candidate=*this;
    if(auto valid=candidate.set_settings({},aspect); !valid) return valid;
    candidate.reset(fixture);
    const bool changed=candidate.revision_!=revision_;
    candidate.revision_=revision_+(changed ? 1 : 0); *this=candidate; return {};
}
Vec3d Camera::backward() const { return {std::cos(pitch_)*std::sin(yaw_),std::sin(pitch_),std::cos(pitch_)*std::cos(yaw_)}; }
Transformd Camera::world() const {
    const Vec3d back=backward(),right=Vec3d::UnitY().cross(back).normalized(),up=back.cross(right);
    Mat4d m=Mat4d::Identity();
    m.block<3,1>(0,0)=right; m.block<3,1>(0,1)=up; m.block<3,1>(0,2)=back; m.block<3,1>(0,3)=eye_;
    return Transformd::from_matrix(m).value();
}
Mat4d Camera::projection(double aspect) const { return perspective_projection(lens(),aspect).value(); }
Mat4d Camera::clip(double aspect) const { return projection(aspect)*world().inverse().value().matrix(); }
Result<geometry::Ray> Camera::ray(const Vec2d& uv,double aspect) const {
    const auto local=perspective_ray(lens(),aspect,uv);
    if(!local) return std::unexpected(local.error());
    return geometry::Ray{eye_,world().transform_direction(local->direction).value(),local->minimum,local->maximum};
}
std::optional<Vec2d> Camera::project(const Vec3d& point,double aspect) const {
    const auto p=perspective_projection(lens(),aspect); if(!p) return {};
    const Vec4d h=(*p)*world().inverse().value().matrix()*Vec4d{point.x(),point.y(),point.z(),1};
    if(!h.allFinite() || h.w()<=0 || h.z()<0 || h.z()>h.w()) return {};
    return Vec2d{(h.x()/h.w()+1)/2,(h.y()/h.w()+1)/2};
}
double Camera::units_per_pixel(const Vec3d& point,double height) const {
    return 2*tangent()*std::max(.05,(eye_-point).dot(backward()))/std::max(1.0,height);
}
void Camera::orbit(double dx,double dy) {
    if(!std::isfinite(dx) || !std::isfinite(dy) || (dx==0 && dy==0)) return;
    Camera candidate=*this; const Vec3d target=pivot(); const auto& n=settings_.navigation;
    candidate.yaw_=std::remainder(yaw_-dx*n.orbit_sensitivity,2*std::numbers::pi);
    candidate.pitch_=std::clamp(pitch_+dy*n.orbit_sensitivity*(n.invert_y ? -1 : 1),-1.55,1.55);
    if(!std::isfinite(candidate.yaw_) || !std::isfinite(candidate.pitch_) || (candidate.yaw_==yaw_ && candidate.pitch_==pitch_)) return;
    candidate.eye_=target+candidate.backward()*distance_;
    if(!candidate.eye_.allFinite()) return;
    candidate.revision_=revision_+1; *this=candidate;
}
void Camera::pan(double dx,double dy,double height) {
    if(!std::isfinite(dx) || !std::isfinite(dy) || !std::isfinite(height) || height<=0 || (dx==0 && dy==0)) return;
    const auto m=world().matrix();
    const Vec3d candidate=eye_+(-dx*m.block<3,1>(0,0)+dy*m.block<3,1>(0,1))*(2*tangent()*distance_/height*settings_.navigation.pan_multiplier);
    if(!candidate.allFinite() || candidate==eye_) return;
    eye_=candidate; ++revision_;
}
void Camera::dolly(double steps) {
    if(!std::isfinite(steps) || steps==0) return;
    const double distance=std::clamp(distance_*std::exp(std::clamp(-steps*settings_.navigation.wheel_sensitivity,-20.0,20.0)),.1,3000.0);
    if(distance==distance_) return;
    const Vec3d candidate=pivot()+backward()*distance;
    if(!candidate.allFinite()) return;
    eye_=candidate; distance_=distance; ++revision_;
}
Result<void> Camera::frame(const geometry::Bounds& bounds,double aspect) {
    if(auto valid=validate(aspect); !valid) return valid;
    if(bounds.empty() || !bounds.minimum.allFinite() || !bounds.maximum.allFinite())
        return std::unexpected(invalid("Frame requires finite nonempty bounds"));
    const Vec3d center=bounds.minimum/2+bounds.maximum/2;
    const auto m=world().matrix();
    double distance=.1,maximum=3000;
    for(int i=0;i<8;++i) {
        const Vec3d corner{i&1 ? bounds.maximum.x() : bounds.minimum.x(),i&2 ? bounds.maximum.y() : bounds.minimum.y(),i&4 ? bounds.maximum.z() : bounds.minimum.z()};
        const Vec3d local=m.topLeftCorner<3,3>().transpose()*(corner-center);
        if(!local.allFinite()) return std::unexpected(invalid("Frame bounds overflow"));
        distance=std::max({distance,local.z()+1.2*std::abs(local.x())/(tangent()*aspect),
            local.z()+1.2*std::abs(local.y())/tangent(),local.z()+near_plane()*1.001});
        maximum=std::min(maximum,local.z()+far_plane()*.999);
    }
    const Vec3d candidate=center+backward()*distance;
    if(!std::isfinite(distance) || distance>maximum || !candidate.allFinite())
        return std::unexpected(invalid("Bounds cannot fit inside the current lens clipping and orbit distance range"));
    if(eye_!=candidate || distance_!=distance) { eye_=candidate; distance_=distance; ++revision_; }
    return {};
}
}

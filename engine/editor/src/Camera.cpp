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
    if(eye_==eye && yaw_==yaw && pitch_==pitch && distance_==distance && standard_view_==StandardView::none) return;
    eye_=eye; yaw_=yaw; pitch_=pitch; distance_=distance; standard_view_=StandardView::none; ++revision_;
}
void Camera::set_environment(bool enabled) {
    const double before=near_plane(); environment_=enabled;
    if(near_plane()!=before) ++revision_;
}
Result<void> Camera::validate(double aspect) const {
    if(projection_==ProjectionMode::orthographic && !orthographic_supported())
        return std::unexpected(Error{ErrorCode::not_supported,"Orthographic needs Path trace, Unlit inspection or Raster without atmosphere/clouds."});
    auto p=projection_result(aspect);
    if(!p) return std::unexpected(p.error());
    if(environment_ && (far_plane()>1e6 || far_plane()/near_plane()>10000 || (*p)(0,0)<.1 || -(*p)(1,1)<.1))
        return std::unexpected(invalid("Raster atmosphere/clouds require far <= 1e6, far/near <= 10000 and projection scales >= 0.1. Adjust FOV/clipping or use Auto."));
    return {};
}
Result<void> Camera::set_settings(const CameraSettings& value,double aspect) {
    const auto& l=value.lens; const auto& n=value.navigation;
    if(!std::isfinite(l.fov_y) || l.fov_y<=1 || l.fov_y>=179 || !std::isfinite(l.near_plane) ||
       !std::isfinite(l.far_plane) || l.near_plane<=0 || l.far_plane<=l.near_plane || !range(l.ortho_height,.01,100000))
        return std::unexpected(invalid("Camera requires 1 < FOV < 179 degrees, 0.01 <= orthographic height <= 100000 m and finite 0 < near < far"));
    if(!range(n.move_speed,.01,1000) || !range(n.fast_multiplier,1,100) || !range(n.slow_multiplier,.01,1) ||
       !range(n.look_sensitivity,.0001,.05) || !range(n.orbit_sensitivity,.0001,.05) ||
       !range(n.pan_multiplier,.01,10) || !range(n.wheel_sensitivity,.01,1))
        return std::unexpected(invalid("Camera navigation setting is outside its supported range"));
    Camera candidate=*this; candidate.settings_=value;
    if(auto valid=candidate.validate(aspect); !valid) return valid;
    const bool changed=(projection_==ProjectionMode::perspective ? fov_y()!=candidate.fov_y() : settings_.lens.ortho_height!=l.ortho_height) || near_plane()!=candidate.near_plane() || far_plane()!=candidate.far_plane();
    settings_=value; if(changed) ++revision_; return {};
}
Result<void> Camera::restore_defaults(bool fixture,double aspect) {
    Camera candidate=*this;
    candidate.projection_=ProjectionMode::perspective; candidate.perspective_mode_=NavigationMode::orbit;
    if(auto valid=candidate.set_settings({},aspect); !valid) return valid;
    candidate.mode_=NavigationMode::orbit;
    candidate.reset(fixture);
    const bool changed=candidate.revision_!=revision_ || projection_!=candidate.projection_;
    candidate.revision_=revision_+(changed ? 1 : 0); *this=candidate; return {};
}
Result<void> Camera::set_projection_mode(ProjectionMode mode,double aspect) {
    if(mode!=ProjectionMode::perspective && mode!=ProjectionMode::orthographic) return std::unexpected(invalid("Unknown projection mode"));
    Camera candidate=*this;
    if(mode!=projection_) {
        if(mode==ProjectionMode::orthographic) {
            candidate.settings_.lens.ortho_height=2*distance_*tangent();
            if(!range(candidate.settings_.lens.ortho_height,.01,100000)) return std::unexpected(invalid("Focus plane height is outside orthographic range"));
            candidate.perspective_mode_=mode_; candidate.mode_=NavigationMode::orbit;
        } else {
            candidate.distance_=settings_.lens.ortho_height/(2*tangent());
            if(!range(candidate.distance_,.1,3000)) return std::unexpected(invalid("Orthographic focus plane cannot fit the perspective orbit distance range"));
            candidate.eye_=pivot()+backward()*candidate.distance_;
            candidate.mode_=perspective_mode_;
        }
        candidate.projection_=mode; candidate.revision_=revision_+1;
    }
    if(!candidate.eye_.allFinite()) return std::unexpected(invalid("Projection switch position overflow"));
    if(auto valid=candidate.validate(aspect); !valid) return valid;
    *this=candidate; return {};
}
Result<void> Camera::set_standard_view(StandardView view,double aspect) {
    if(projection_!=ProjectionMode::orthographic) return std::unexpected(invalid("Six views require Orthographic Orbit"));
    if(view!=StandardView::none && view==standard_view_) return validate(aspect);
    Camera candidate=*this;
    switch(view) {
    case StandardView::front: candidate.yaw_=0; candidate.pitch_=0; break;
    case StandardView::back: candidate.yaw_=std::numbers::pi; candidate.pitch_=0; break;
    case StandardView::left: candidate.yaw_=-std::numbers::pi/2; candidate.pitch_=0; break;
    case StandardView::right: candidate.yaw_=std::numbers::pi/2; candidate.pitch_=0; break;
    case StandardView::top: candidate.yaw_=0; candidate.pitch_=std::numbers::pi/2; break;
    case StandardView::bottom: candidate.yaw_=0; candidate.pitch_=-std::numbers::pi/2; break;
    default: return std::unexpected(invalid("Unknown standard view"));
    }
    candidate.standard_view_=view; candidate.eye_=pivot()+candidate.backward()*distance_;
    if(!candidate.eye_.allFinite()) return std::unexpected(invalid("Standard view position overflow"));
    if(auto valid=candidate.validate(aspect); !valid) return valid;
    candidate.revision_=revision_+(candidate.world().matrix()!=world().matrix() ? 1 : 0);
    *this=candidate; return {};
}
Vec3d Camera::backward() const {
    switch(standard_view_) {
    case StandardView::front: return {0,0,1}; case StandardView::back: return {0,0,-1};
    case StandardView::left: return {-1,0,0}; case StandardView::right: return {1,0,0};
    case StandardView::top: return {0,1,0}; case StandardView::bottom: return {0,-1,0};
    default: return {std::cos(pitch_)*std::sin(yaw_),std::sin(pitch_),std::cos(pitch_)*std::cos(yaw_)};
    }
}
Transformd Camera::world() const {
    const Vec3d back=backward();
    const Vec3d right=standard_view_==StandardView::top || standard_view_==StandardView::bottom ? Vec3d{1,0,0} :
        standard_view_!=StandardView::none ? Vec3d{Vec3d::UnitY().cross(back)} : Vec3d{std::cos(yaw_),0,-std::sin(yaw_)};
    const Vec3d up=back.cross(right);
    Mat4d m=Mat4d::Identity();
    m.block<3,1>(0,0)=right; m.block<3,1>(0,1)=up; m.block<3,1>(0,2)=back; m.block<3,1>(0,3)=eye_;
    return Transformd::from_matrix(m).value();
}
Result<Mat4d> Camera::projection_result(double aspect) const {
    return projection_==ProjectionMode::perspective ? perspective_projection(lens(),aspect) : orthographic_projection(ortho_lens(),aspect);
}
Mat4d Camera::projection(double aspect) const { return projection_result(aspect).value(); }
Mat4d Camera::clip(double aspect) const { return projection(aspect)*world().inverse().value().matrix(); }
Result<geometry::Ray> Camera::ray(const Vec2d& uv,double aspect) const {
    if(projection_==ProjectionMode::orthographic) {
        const auto local=orthographic_ray(ortho_lens(),aspect,uv); if(!local) return std::unexpected(local.error());
        auto origin=world().transform_point(local->origin); if(!origin) return std::unexpected(origin.error());
        return geometry::Ray{*origin,-backward(),local->minimum,local->maximum};
    }
    const auto local=perspective_ray(lens(),aspect,uv);
    if(!local) return std::unexpected(local.error());
    return geometry::Ray{eye_,world().transform_direction(local->direction).value(),local->minimum,local->maximum};
}
std::optional<Vec2d> Camera::project(const Vec3d& point,double aspect) const {
    const auto p=projection_result(aspect); if(!p) return {};
    const Vec4d h=(*p)*world().inverse().value().matrix()*Vec4d{point.x(),point.y(),point.z(),1};
    if(!h.allFinite() || h.w()<=0 || h.z()<0 || h.z()>h.w()) return {};
    return Vec2d{(h.x()/h.w()+1)/2,(h.y()/h.w()+1)/2};
}
double Camera::units_per_pixel(const Vec3d& point,double height) const {
    if(projection_==ProjectionMode::orthographic) return settings_.lens.ortho_height/std::max(1.0,height);
    return 2*tangent()*std::max(.05,(eye_-point).dot(backward()))/std::max(1.0,height);
}
void Camera::orbit(double dx,double dy) {
    if(!std::isfinite(dx) || !std::isfinite(dy) || (dx==0 && dy==0)) return;
    Camera candidate=*this; const Vec3d target=pivot(); const auto& n=settings_.navigation;
    candidate.standard_view_=StandardView::none;
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
    const Vec3d candidate=eye_+(-dx*m.block<3,1>(0,0)+dy*m.block<3,1>(0,1))*(units_per_pixel(pivot(),height)*settings_.navigation.pan_multiplier);
    if(!candidate.allFinite() || candidate==eye_) return;
    eye_=candidate; ++revision_;
}
void Camera::dolly(double steps) {
    if(!std::isfinite(steps) || steps==0) return;
    if(projection_==ProjectionMode::orthographic) {
        const double height=std::clamp(settings_.lens.ortho_height*std::exp(std::clamp(-steps*settings_.navigation.wheel_sensitivity,-20.0,20.0)),.01,100000.0);
        if(height!=settings_.lens.ortho_height) { settings_.lens.ortho_height=height; ++revision_; }
        return;
    }
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
    double distance=.1,maximum=3000,height=.01;
    for(int i=0;i<8;++i) {
        const Vec3d corner{i&1 ? bounds.maximum.x() : bounds.minimum.x(),i&2 ? bounds.maximum.y() : bounds.minimum.y(),i&4 ? bounds.maximum.z() : bounds.minimum.z()};
        const Vec3d local=m.topLeftCorner<3,3>().transpose()*(corner-center);
        if(!local.allFinite()) return std::unexpected(invalid("Frame bounds overflow"));
        if(projection_==ProjectionMode::orthographic) {
            height=std::max({height,2.4*std::abs(local.x())/aspect,2.4*std::abs(local.y())});
            distance=std::max(distance,local.z()+near_plane()*1.001);
        } else distance=std::max({distance,local.z()+1.2*std::abs(local.x())/(tangent()*aspect),
            local.z()+1.2*std::abs(local.y())/tangent(),local.z()+near_plane()*1.001});
        maximum=std::min(maximum,local.z()+far_plane()*.999);
    }
    const Vec3d candidate=center+backward()*distance;
    if(!std::isfinite(distance) || distance>maximum || !range(height,.01,100000) || !candidate.allFinite())
        return std::unexpected(invalid("Bounds cannot fit inside the current lens clipping and orbit distance range"));
    const bool resized=projection_==ProjectionMode::orthographic && settings_.lens.ortho_height!=height;
    if(eye_!=candidate || distance_!=distance || resized) {
        eye_=candidate; distance_=distance; if(resized) settings_.lens.ortho_height=height; ++revision_;
    }
    return {};
}
}

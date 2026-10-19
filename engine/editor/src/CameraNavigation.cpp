#include <dk/editor/CameraNavigation.hpp>
#include <algorithm>
#include <numbers>

namespace dk::editor {
Result<void> Camera::set_navigation_mode(NavigationMode mode) {
    if(mode!=NavigationMode::orbit && mode!=NavigationMode::fly && mode!=NavigationMode::walk)
        return std::unexpected(Error{ErrorCode::invalid_argument,"Unknown navigation mode"});
    if(projection_==ProjectionMode::orthographic && mode!=NavigationMode::orbit)
        return std::unexpected(Error{ErrorCode::not_supported,"Orthographic supports Orbit only; return to Perspective for Fly/Walk"});
    mode_=mode; return {};
}
Result<void> CameraNavigation::update(Camera& camera,const NavigationInput& input) {
    if(!active_ || camera.mode_==NavigationMode::orbit) return {};
    const auto invalid=[] { return std::unexpected(Error{ErrorCode::invalid_argument,"Navigation requires finite input, nonnegative dt and movement axes in [-1,1]"}); };
    if(!input.look.allFinite() || !input.move.allFinite() || (input.move.cwiseAbs().array()>1).any() ||
       !std::isfinite(input.seconds) || input.seconds<0 || !std::isfinite(input.wheel)) return invalid();
    Camera candidate=camera;
    auto& n=candidate.settings_.navigation;
    n.move_speed=std::clamp(n.move_speed*std::exp(std::clamp(input.wheel*n.wheel_sensitivity,-20.0,20.0)),.01,1000.0);
    if(!first_) {
        if(!input.look.isZero()) {
            candidate.standard_view_=StandardView::none;
            candidate.yaw_=std::remainder(camera.yaw_-input.look.x()*n.look_sensitivity,2*std::numbers::pi);
            candidate.pitch_=std::clamp(camera.pitch_+input.look.y()*n.look_sensitivity*(n.invert_y ? -1 : 1),-1.55,1.55);
        }
        if(!std::isfinite(candidate.yaw_) || !std::isfinite(candidate.pitch_)) return invalid();
        const Vec3d right{std::cos(candidate.yaw_),0,-std::sin(candidate.yaw_)};
        const Vec3d forward=candidate.mode_==NavigationMode::walk ?
            Vec3d{-std::sin(candidate.yaw_),0,-std::cos(candidate.yaw_)} : Vec3d{-candidate.backward()};
        Vec3d direction=right*input.move.x()+forward*input.move.z();
        if(candidate.mode_==NavigationMode::fly) direction.y()+=input.move.y();
        if(direction.squaredNorm()>0) direction.normalize();
        const double speed=n.move_speed*(input.slow ? n.slow_multiplier : input.fast ? n.fast_multiplier : 1);
        candidate.eye_+=direction*(speed*std::min(input.seconds,.05));
        if(!candidate.eye_.allFinite()) return invalid();
    }
    const bool changed=candidate.eye_!=camera.eye_ || candidate.yaw_!=camera.yaw_ || candidate.pitch_!=camera.pitch_;
    candidate.revision_=camera.revision_+(changed ? 1 : 0);
    camera=candidate; first_=false; return {};
}
}

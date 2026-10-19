#include <dk/math/Projection.hpp>
#include <cmath>

namespace dk {
Result<Mat4d> orthographic_projection(const OrthographicLens& lens,double aspect) {
    const auto [height,near,far]=lens;
    if(!std::isfinite(height) || height<=0 || !std::isfinite(aspect) || aspect<=0 ||
       !std::isfinite(near) || near<=0 || !std::isfinite(far) || far<=near)
        return std::unexpected(Error{ErrorCode::invalid_argument,"Orthographic requires finite positive height/aspect and 0 < near < far"});
    Mat4d result=Mat4d::Identity();
    result(0,0)=(2/height)/aspect; result(1,1)=-2/height;
    result(2,2)=-1/(far-near); result(2,3)=near*result(2,2);
    if(!result.allFinite() || result(0,0)<=0 || result(1,1)>=0 || result(2,2)>=0 || result(2,3)>=0)
        return std::unexpected(Error{ErrorCode::invalid_argument,"Orthographic matrix is not representable"});
    return result;
}
Result<OrthographicRay> orthographic_ray(const OrthographicLens& lens,double aspect,const Vec2d& uv) {
    if(!uv.allFinite() || (uv.array()<0).any() || (uv.array()>1).any())
        return std::unexpected(Error{ErrorCode::invalid_argument,"Orthographic ray requires UV in [0,1]"});
    const auto p=orthographic_projection(lens,aspect); if(!p) return std::unexpected(p.error());
    const Vec3d origin{(2*uv.x()-1)/(*p)(0,0),(2*uv.y()-1)/(*p)(1,1),-lens.near_plane};
    if(!origin.allFinite()) return std::unexpected(Error{ErrorCode::invalid_argument,"Orthographic ray origin overflow"});
    return OrthographicRay{origin,{0,0,-1},0,lens.far_plane-lens.near_plane};
}
Result<Mat4d> perspective_projection(const PerspectiveLens& lens,double aspect) {
    const auto [angle,near,far]=lens;
    if(!std::isfinite(angle) || angle<=0 || angle>=std::numbers::pi ||
       !std::isfinite(aspect) || aspect<=0 || !std::isfinite(near) || near<=0 || !std::isfinite(far) || far<=near)
        return std::unexpected(Error{ErrorCode::invalid_argument,"Perspective requires finite FOV in (0,pi), positive aspect and 0 < near < far"});
    const double f=1/std::tan(angle/2);
    Mat4d result=Mat4d::Zero();
    result(0,0)=f/aspect; result(1,1)=-f;
    result(2,2)=far/(near-far); result(2,3)=near*result(2,2); result(3,2)=-1;
    if(!result.allFinite() || result(0,0)<=0 || result(1,1)>=0 || result(2,2)>=-1 || result(2,3)>=0)
        return std::unexpected(Error{ErrorCode::invalid_argument,"Perspective clipping range or matrix is not representable"});
    return result;
}
Result<PerspectiveRay> perspective_ray(const PerspectiveLens& lens,double aspect,const Vec2d& uv) {
    if(!uv.allFinite() || (uv.array()<0).any() || (uv.array()>1).any())
        return std::unexpected(Error{ErrorCode::invalid_argument,"Perspective ray requires UV in [0,1]"});
    const auto projection=perspective_projection(lens,aspect);
    if(!projection) return std::unexpected(projection.error());
    const Vec3d local{(2*uv.x()-1)/(*projection)(0,0),(2*uv.y()-1)/(*projection)(1,1),-1};
    const double length=local.stableNorm(),minimum=lens.near_plane*length,maximum=lens.far_plane*length;
    if(!local.allFinite() || !std::isfinite(length) || !std::isfinite(maximum) || minimum<=0 || maximum<=minimum)
        return std::unexpected(Error{ErrorCode::invalid_argument,"Perspective ray clipping distance overflow"});
    return PerspectiveRay{local/length,minimum,maximum};
}
}

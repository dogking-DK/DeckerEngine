#pragma once
#include <dk/render/ScenePipeline.hpp>
#include <array>
#include <cmath>

namespace dk::render::detail {
struct PerspectiveDepth { double near_m,far_m; };
inline Result<PerspectiveDepth> perspective_depth(const ViewDescription& view) {
    const auto& p=view.projection; const Mat3d rotation=view.camera_world.matrix().topLeftCorner<3,3>();
    Mat4d expected=Mat4d::Zero();
    expected(0,0)=p(0,0); expected(1,1)=p(1,1); expected(0,2)=p(0,2); expected(1,2)=p(1,2);
    expected(2,2)=p(2,2); expected(2,3)=p(2,3); expected(3,2)=-1;
    const double near_m=p(2,3)/p(2,2),far_m=p(2,3)/(p(2,2)+1);
    if(!p.allFinite() || p!=expected || p(0,0)<=0 || p(1,1)==0 ||
        !std::isfinite(near_m) || !std::isfinite(far_m) || near_m<=0 || far_m<=near_m ||
        // p22 + 1 loses precision at the far/near boundary; admit double rounding only.
        far_m>1e6*(1+1e-10) || far_m/near_m>10000*(1+1e-10) ||
        !(rotation.transpose()*rotation).isApprox(Mat3d::Identity(),1e-10) || std::abs(rotation.determinant()-1)>1e-10 ||
        (p.topLeftCorner<2,2>().diagonal().cwiseAbs().array()<.1).any() ||
        std::abs(p(0,2))>2 || std::abs(p(1,2))>2)
        return std::unexpected(Error{ErrorCode::not_supported,"atmosphere requires a rigid camera and finite forward-Z perspective (far <= 1e6 m, far/near <= 10000, bounded field of view)"});
    return PerspectiveDepth{near_m,far_m};
}
inline Result<Mat3d> normal_matrix(const Transformd& world) {
    auto inverse=world.inverse();
    if(!inverse) return std::unexpected(inverse.error().with_context("Lambert normal transform"));
    Mat3d normal=inverse->matrix().topLeftCorner<3,3>().transpose();
    normal/=normal.cwiseAbs().maxCoeff(); // Positive common scale preserves direction and avoids float overflow.
    if(!normal.allFinite()) return std::unexpected(Error{ErrorCode::invalid_argument,"nonfinite normal matrix"});
    return normal;
}
inline Result<std::array<float,16>> clip_rows(const RenderView& view, const Transformd& world) {
    const Mat4d matrix = view.world_to_clip() * world.matrix();
    std::array<float,16> result{};
    if (!matrix.allFinite() || (matrix.array().abs() > static_cast<double>(std::numeric_limits<float>::max())).any())
        return std::unexpected(Error{ErrorCode::invalid_argument,"world-to-clip transform cannot be represented as finite float"});
    for (int row = 0; row < 4; ++row) for (int column = 0; column < 4; ++column)
        result[static_cast<std::size_t>(row*4+column)] = static_cast<float>(matrix(row,column));
    return result;
}
}

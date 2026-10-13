#pragma once
#include <dk/render/ScenePipeline.hpp>
#include <array>
#include <cmath>

namespace dk::render::detail {
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

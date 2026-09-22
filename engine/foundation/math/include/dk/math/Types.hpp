#pragma once

#include <Eigen/Core>
#include <Eigen/Geometry>

namespace dk {

// Right-handed coordinates, column vectors, explicit column-major matrices.
// Eigen default construction does not initialize coefficients: use Zero()/Identity().
using Vec2f = Eigen::Vector2f;
using Vec3f = Eigen::Vector3f;
using Vec4f = Eigen::Vector4f;
using Vec2d = Eigen::Vector2d;
using Vec3d = Eigen::Vector3d;
using Vec4d = Eigen::Vector4d;
using Mat3f = Eigen::Matrix<float, 3, 3, Eigen::ColMajor>;
using Mat4f = Eigen::Matrix<float, 4, 4, Eigen::ColMajor>;
using Mat3d = Eigen::Matrix<double, 3, 3, Eigen::ColMajor>;
using Mat4d = Eigen::Matrix<double, 4, 4, Eigen::ColMajor>;
// Constructors take (w, x, y, z); coeffs() stores (x, y, z, w).
using Quatf = Eigen::Quaternionf;
using Quatd = Eigen::Quaterniond;

} // namespace dk

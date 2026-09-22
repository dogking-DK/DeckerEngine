#pragma once

#include <dk/core/Result.hpp>
#include <dk/math/Types.hpp>

#include <concepts>
#include <utility>

namespace dk {

template <typename Scalar>
concept TransformScalar = std::same_as<Scalar, float> || std::same_as<Scalar, double>;

// Editable local data. Validation and quaternion normalization occur in from_trs().
template <TransformScalar Scalar>
struct BasicTrs {
    Eigen::Matrix<Scalar, 3, 1> translation = Eigen::Matrix<Scalar, 3, 1>::Zero();
    Eigen::Quaternion<Scalar> rotation = Eigen::Quaternion<Scalar>::Identity();
    Eigen::Matrix<Scalar, 3, 1> scale = Eigen::Matrix<Scalar, 3, 1>::Ones();
};

// A finite affine value. Full matrix storage preserves shear after composition.
template <TransformScalar Scalar>
class BasicTransform {
public:
    using Vector3 = Eigen::Matrix<Scalar, 3, 1>;
    using Matrix4 = Eigen::Matrix<Scalar, 4, 4, Eigen::ColMajor>;
    using TrsType = BasicTrs<Scalar>;

    BasicTransform() = default;

    // Apply scale, then rotation, then translation. Zero and negative scales are valid.
    [[nodiscard]] static Result<BasicTransform> from_trs(const TrsType& trs);
    // Rejects projective matrices; the last row must be exactly [0, 0, 0, 1].
    [[nodiscard]] static Result<BasicTransform> from_matrix(const Matrix4& matrix);

    [[nodiscard]] const Matrix4& matrix() const noexcept { return matrix_; }

    // parent.compose(local) applies local first. Does not decompose back into TRS.
    [[nodiscard]] Result<BasicTransform> compose(const BasicTransform& local) const;
    // Rejects singular/numerically rank-deficient linear parts and nonfinite results.
    [[nodiscard]] Result<BasicTransform> inverse() const;

    [[nodiscard]] Result<Vector3> transform_point(const Vector3& point) const;
    // Includes scale/shear but not translation. Not a normal-vector transform.
    [[nodiscard]] Result<Vector3> transform_direction(const Vector3& direction) const;

private:
    explicit BasicTransform(Matrix4 matrix) : matrix_{std::move(matrix)} {}
    Matrix4 matrix_ = Matrix4::Identity();
};

using Trsf = BasicTrs<float>;
using Trsd = BasicTrs<double>;
using Trs = Trsf;
using Transformf = BasicTransform<float>;
using Transformd = BasicTransform<double>;
using Transform = Transformf;

extern template class BasicTransform<float>;
extern template class BasicTransform<double>;

} // namespace dk

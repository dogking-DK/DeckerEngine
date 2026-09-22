#include <dk/math/Transform.hpp>

#include <dk/math/Math.hpp>

#include <Eigen/LU>

#include <limits>
#include <string>
#include <string_view>
#include <utility>

namespace dk {
namespace {

std::unexpected<Error> transform_error(
    ErrorCode code, std::string_view message, std::string_view operation)
{
    return std::unexpected(Error{code, std::string{message}, {std::string{operation}}});
}

} // namespace

template <TransformScalar Scalar>
Result<BasicTransform<Scalar>> BasicTransform<Scalar>::from_trs(const TrsType& trs)
{
    if (!trs.translation.allFinite() || !trs.scale.allFinite()) {
        return transform_error(ErrorCode::invalid_argument,
            "Translation and scale must be finite", "Transform::from_trs");
    }
    const auto rotation = normalize_quaternion(trs.rotation);
    if (!rotation) {
        return std::unexpected(rotation.error().with_context("Transform::from_trs"));
    }
    Matrix4 matrix = Matrix4::Identity();
    matrix.template block<3, 3>(0, 0) = rotation->toRotationMatrix() * trs.scale.asDiagonal();
    matrix.template block<3, 1>(0, 3) = trs.translation;
    if (!matrix.allFinite()) {
        return transform_error(ErrorCode::invalid_state,
            "TRS matrix exceeds the scalar range", "Transform::from_trs");
    }
    return BasicTransform{std::move(matrix)};
}

template <TransformScalar Scalar>
Result<BasicTransform<Scalar>> BasicTransform<Scalar>::from_matrix(const Matrix4& matrix)
{
    if (!matrix.allFinite() || matrix(3, 0) != Scalar{0} || matrix(3, 1) != Scalar{0}
        || matrix(3, 2) != Scalar{0} || matrix(3, 3) != Scalar{1}) {
        return transform_error(ErrorCode::invalid_argument,
            "Matrix must be finite and affine with last row [0, 0, 0, 1]", "Transform::from_matrix");
    }
    return BasicTransform{matrix};
}

template <TransformScalar Scalar>
Result<BasicTransform<Scalar>> BasicTransform<Scalar>::compose(const BasicTransform& local) const
{
    Matrix4 combined = matrix_ * local.matrix_;
    if (!combined.allFinite()) {
        return transform_error(ErrorCode::invalid_state,
            "Composed matrix exceeds the scalar range", "Transform::compose");
    }
    return BasicTransform{std::move(combined)};
}

template <TransformScalar Scalar>
Result<BasicTransform<Scalar>> BasicTransform<Scalar>::inverse() const
{
    using Matrix3 = Eigen::Matrix<Scalar, 3, 3, Eigen::ColMajor>;
    const Matrix3 linear = matrix_.template block<3, 3>(0, 0);
    const Scalar scale = linear.cwiseAbs().maxCoeff();
    if (scale == Scalar{0}) {
        return transform_error(ErrorCode::invalid_state,
            "Transform has a singular linear part", "Transform::inverse");
    }
    Matrix3 scaled;
    for (Eigen::Index column = 0; column < 3; ++column) {
        for (Eigen::Index row = 0; row < 3; ++row) {
            scaled(row, column) = linear(row, column) / scale;
        }
    }
    Eigen::FullPivLU<Matrix3> decomposition{scaled};
    decomposition.setThreshold(Scalar{64} * std::numeric_limits<Scalar>::epsilon());
    if (!decomposition.isInvertible()) {
        return transform_error(ErrorCode::invalid_state,
            "Transform is singular or numerically rank deficient", "Transform::inverse");
    }

    const Matrix3 scaled_inverse = decomposition.inverse();
    Matrix3 inverse_linear;
    for (Eigen::Index column = 0; column < 3; ++column) {
        for (Eigen::Index row = 0; row < 3; ++row) {
            // Divide directly rather than form a reciprocal that may overflow.
            inverse_linear(row, column) = scaled_inverse(row, column) / scale;
        }
    }
    if (!inverse_linear.allFinite()) {
        return transform_error(ErrorCode::invalid_state,
            "Inverse linear part exceeds the scalar range", "Transform::inverse");
    }

    Matrix4 result = Matrix4::Identity();
    result.template block<3, 3>(0, 0) = inverse_linear;
    result.template block<3, 1>(0, 3) = -(inverse_linear * matrix_.template block<3, 1>(0, 3));
    if (!result.allFinite()) {
        return transform_error(ErrorCode::invalid_state,
            "Inverse translation exceeds the scalar range", "Transform::inverse");
    }
    return BasicTransform{std::move(result)};
}

template <TransformScalar Scalar>
Result<typename BasicTransform<Scalar>::Vector3>
BasicTransform<Scalar>::transform_point(const Vector3& point) const
{
    if (!point.allFinite()) {
        return transform_error(ErrorCode::invalid_argument,
            "Point must be finite", "Transform::transform_point");
    }
    Vector3 result = matrix_.template block<3, 3>(0, 0) * point
        + matrix_.template block<3, 1>(0, 3);
    if (!result.allFinite()) {
        return transform_error(ErrorCode::invalid_state,
            "Transformed point exceeds the scalar range", "Transform::transform_point");
    }
    return result;
}

template <TransformScalar Scalar>
Result<typename BasicTransform<Scalar>::Vector3>
BasicTransform<Scalar>::transform_direction(const Vector3& direction) const
{
    if (!direction.allFinite()) {
        return transform_error(ErrorCode::invalid_argument,
            "Direction must be finite", "Transform::transform_direction");
    }
    Vector3 result = matrix_.template block<3, 3>(0, 0) * direction;
    if (!result.allFinite()) {
        return transform_error(ErrorCode::invalid_state,
            "Transformed direction exceeds the scalar range", "Transform::transform_direction");
    }
    return result;
}

template class BasicTransform<float>;
template class BasicTransform<double>;

} // namespace dk

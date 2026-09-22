#include <dk/math/Math.hpp>

#include <cmath>
#include <string>
#include <string_view>
#include <utility>

namespace dk {
namespace {

template <typename Vector>
Result<Vector> normalize_coefficients(const Vector& value, std::string_view operation)
{
    if (!value.allFinite()) {
        return std::unexpected(Error{ErrorCode::invalid_argument,
            "Components must be finite", {std::string{operation}}});
    }
    const auto scale = value.cwiseAbs().maxCoeff();
    if (scale == 0) {
        return std::unexpected(Error{ErrorCode::invalid_argument,
            "Cannot normalize zero components", {std::string{operation}}});
    }

    // Scale first: the original squared norm can overflow or underflow.
    // Scalar division also avoids forming an overflowing reciprocal for tiny values.
    Vector scaled;
    for (Eigen::Index index = 0; index < value.size(); ++index) {
        scaled[index] = value[index] / scale;
    }
    Vector result = scaled.normalized();
    return result;
}

template <typename Quaternion>
Result<Quaternion> normalize_rotation(const Quaternion& value)
{
    using Scalar = typename Quaternion::Scalar;
    const Eigen::Matrix<Scalar, 4, 1> coefficients = value.coeffs();
    auto normalized = normalize_coefficients(coefficients, "normalize_quaternion");
    if (!normalized) {
        return std::unexpected(std::move(normalized.error()));
    }
    Quaternion result;
    result.coeffs() = *normalized;
    return result;
}

template <typename Scalar>
Result<Eigen::Quaternion<Scalar>> axis_angle(
    const Eigen::Matrix<Scalar, 3, 1>& axis, Scalar angle)
{
    if (!std::isfinite(angle)) {
        return std::unexpected(Error{ErrorCode::invalid_argument,
            "Angle must be finite", {"rotation_from_axis_angle"}});
    }
    auto normalized = normalize_coefficients(axis, "rotation_from_axis_angle");
    if (!normalized) {
        return std::unexpected(std::move(normalized.error()));
    }
    const Eigen::Quaternion<Scalar> rotation{Eigen::AngleAxis<Scalar>{angle, *normalized}};
    return normalize_rotation(rotation);
}

} // namespace

Result<Vec3f> normalize_vector(const Vec3f& vector)
{
    return normalize_coefficients(vector, "normalize_vector");
}

Result<Vec3d> normalize_vector(const Vec3d& vector)
{
    return normalize_coefficients(vector, "normalize_vector");
}

Result<Quatf> normalize_quaternion(const Quatf& quaternion)
{
    return normalize_rotation(quaternion);
}

Result<Quatd> normalize_quaternion(const Quatd& quaternion)
{
    return normalize_rotation(quaternion);
}

Result<Quatf> rotation_from_axis_angle(const Vec3f& axis, float angle_radians)
{
    return axis_angle(axis, angle_radians);
}

Result<Quatd> rotation_from_axis_angle(const Vec3d& axis, double angle_radians)
{
    return axis_angle(axis, angle_radians);
}

} // namespace dk

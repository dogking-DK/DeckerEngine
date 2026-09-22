#pragma once

#include <dk/core/Result.hpp>
#include <dk/math/Types.hpp>

#include <concepts>
#include <numbers>

namespace dk {

template <std::floating_point Scalar>
[[nodiscard]] constexpr Scalar radians(Scalar degrees_value) noexcept
{
    return degrees_value * (std::numbers::pi_v<Scalar> / Scalar{180});
}

template <std::floating_point Scalar>
[[nodiscard]] constexpr Scalar degrees(Scalar radians_value) noexcept
{
    return radians_value * (Scalar{180} / std::numbers::pi_v<Scalar>);
}

// Finite, nonzero input is required. No application-specific epsilon is imposed.
[[nodiscard]] Result<Vec3f> normalize_vector(const Vec3f& vector);
[[nodiscard]] Result<Vec3d> normalize_vector(const Vec3d& vector);
[[nodiscard]] Result<Quatf> normalize_quaternion(const Quatf& quaternion);
[[nodiscard]] Result<Quatd> normalize_quaternion(const Quatd& quaternion);

// Right-handed active rotation. Axis need not be unit length, but must be nonzero.
[[nodiscard]] Result<Quatf> rotation_from_axis_angle(const Vec3f& axis, float angle_radians);
[[nodiscard]] Result<Quatd> rotation_from_axis_angle(const Vec3d& axis, double angle_radians);

} // namespace dk

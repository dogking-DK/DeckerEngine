#include <dk/math/Math.hpp>

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numbers>
#include <type_traits>
#include <vector>

namespace {

template <typename Scalar>
using Vector3 = std::conditional_t<std::is_same_v<Scalar, float>, dk::Vec3f, dk::Vec3d>;
template <typename Scalar>
using Vector4 = std::conditional_t<std::is_same_v<Scalar, float>, dk::Vec4f, dk::Vec4d>;
template <typename Scalar>
using Matrix4 = std::conditional_t<std::is_same_v<Scalar, float>, dk::Mat4f, dk::Mat4d>;
template <typename Scalar>
using Quaternion = std::conditional_t<std::is_same_v<Scalar, float>, dk::Quatf, dk::Quatd>;
template <typename Scalar>
constexpr Scalar tolerance = Scalar{64} * std::numeric_limits<Scalar>::epsilon();

static_assert(std::is_same_v<dk::Vec2f, Eigen::Vector2f>);
static_assert(std::is_same_v<dk::Vec2d, Eigen::Vector2d>);
static_assert(std::is_same_v<dk::Mat3f, Eigen::Matrix3f>);
static_assert(std::is_same_v<dk::Mat3d, Eigen::Matrix3d>);
static_assert(!dk::Mat4f::IsRowMajor && !dk::Mat4d::IsRowMajor);
static_assert(dk::radians(180.0) == std::numbers::pi);
static_assert(dk::degrees(0.0f) == 0.0f);

} // namespace

TEMPLATE_TEST_CASE("angle conversion preserves sign and full turns", "[math]", float, double)
{
    using S = TestType;
    for (const S angle : std::array<S, 5>{S{-720}, S{-90}, S{0}, S{45}, S{360}}) {
        REQUIRE(std::abs(dk::degrees(dk::radians(angle)) - angle) <= tolerance<S> * S{720});
    }
    REQUIRE(std::abs(dk::radians(S{90}) - std::numbers::pi_v<S> / S{2}) <= tolerance<S>);
    REQUIRE(std::isinf(dk::radians(std::numeric_limits<S>::infinity())));
    REQUIRE(std::isnan(dk::degrees(std::numeric_limits<S>::quiet_NaN())));
}

TEMPLATE_TEST_CASE("coordinates use right handed column vector composition", "[math]", float, double)
{
    using S = TestType;
    using V = Vector3<S>;
    using M = Matrix4<S>;
    const V right = V::UnitX();
    const V up = V::UnitY();
    REQUIRE(right.cross(up).isApprox(V::UnitZ(), tolerance<S>));

    M translation = M::Identity();
    translation.template block<3, 1>(0, 3) = V{S{10}, S{20}, S{30}};
    M scale = M::Identity();
    scale(0, 0) = S{2};
    scale(1, 1) = S{3};
    scale(2, 2) = S{4};
    const M combined = translation * scale;
    const Vector4<S> point{S{1}, S{2}, S{3}, S{1}};
    const Vector4<S> direction{S{1}, S{2}, S{3}, S{0}};
    const Vector4<S> moved = combined * point;
    const Vector4<S> directed = combined * direction;
    REQUIRE(moved.isApprox(Vector4<S>{S{12}, S{26}, S{42}, S{1}}, tolerance<S>));
    REQUIRE(directed.isApprox(Vector4<S>{S{2}, S{6}, S{12}, S{0}}, tolerance<S>));
    REQUIRE(translation.data()[12] == S{10});
    REQUIRE(translation.data()[13] == S{20});
    REQUIRE(translation.data()[14] == S{30});
}

TEMPLATE_TEST_CASE("vector normalization handles finite extreme magnitudes", "[math]", float, double)
{
    using S = TestType;
    using V = Vector3<S>;
    const V original{S{3}, S{4}, S{0}};
    const auto unit = dk::normalize_vector(original);
    REQUIRE(unit.has_value());
    REQUIRE(unit->isApprox(V{S{3} / S{5}, S{4} / S{5}, S{0}}, tolerance<S>));
    REQUIRE(original == V{S{3}, S{4}, S{0}});
    for (const S magnitude : std::array<S, 3>{std::numeric_limits<S>::max(),
             std::numeric_limits<S>::min(), std::numeric_limits<S>::denorm_min()}) {
        const V extreme{magnitude, -magnitude, magnitude};
        const auto normalized = dk::normalize_vector(extreme);
        REQUIRE(normalized.has_value());
        REQUIRE(normalized->allFinite());
        REQUIRE(std::abs(normalized->norm() - S{1}) <= tolerance<S>);
        const V expected = V{S{1}, S{-1}, S{1}}.normalized();
        REQUIRE(normalized->isApprox(expected, tolerance<S>));
    }
}

TEMPLATE_TEST_CASE("vector normalization rejects zero and nonfinite inputs", "[math]", float, double)
{
    using S = TestType;
    using V = Vector3<S>;
    const std::array<V, 4> invalid{V::Zero(), V{S{0}, -S{0}, S{0}},
        V{std::numeric_limits<S>::infinity(), S{1}, S{0}},
        V{S{1}, S{0}, std::numeric_limits<S>::quiet_NaN()}};
    for (const auto& value : invalid) {
        const auto result = dk::normalize_vector(value);
        REQUIRE_FALSE(result.has_value());
        REQUIRE(result.error().code == dk::ErrorCode::invalid_argument);
        REQUIRE_FALSE(result.error().context.empty());
    }
}

TEMPLATE_TEST_CASE("quaternion normalization preserves coefficient order and rotation", "[math]", float, double)
{
    using S = TestType;
    using Q = Quaternion<S>;
    const Q original{S{1}, S{2}, S{3}, S{4}};
    REQUIRE(original.coeffs() == Vector4<S>{S{2}, S{3}, S{4}, S{1}});
    const auto unit = dk::normalize_quaternion(original);
    REQUIRE(unit.has_value());
    REQUIRE(std::abs(unit->norm() - S{1}) <= tolerance<S>);
    REQUIRE(std::abs(unit->w() - S{1} / std::sqrt(S{30})) <= tolerance<S>);

    for (const S magnitude : std::array<S, 3>{std::numeric_limits<S>::max(),
             std::numeric_limits<S>::min(), std::numeric_limits<S>::denorm_min()}) {
        const Q extreme{magnitude, -magnitude, magnitude, -magnitude};
        const auto normalized = dk::normalize_quaternion(extreme);
        REQUIRE(normalized.has_value());
        REQUIRE(normalized->coeffs().allFinite());
        REQUIRE(normalized->coeffs().isApprox(Vector4<S>{-S{0.5}, S{0.5}, -S{0.5}, S{0.5}}, tolerance<S>));
    }

    Q negated;
    negated.coeffs() = -original.coeffs();
    const auto opposite = dk::normalize_quaternion(negated);
    REQUIRE(opposite.has_value());
    REQUIRE(opposite->toRotationMatrix().isApprox(unit->toRotationMatrix(), tolerance<S>));
    REQUIRE(opposite->coeffs().isApprox(-unit->coeffs(), tolerance<S>));
}

TEMPLATE_TEST_CASE("quaternion normalization rejects zero and nonfinite inputs", "[math]", float, double)
{
    using S = TestType;
    using Q = Quaternion<S>;
    const std::array<Q, 3> invalid{Q{S{0}, S{0}, S{0}, S{0}},
        Q{std::numeric_limits<S>::quiet_NaN(), S{0}, S{0}, S{0}},
        Q{S{1}, S{0}, std::numeric_limits<S>::infinity(), S{0}}};
    for (const auto& value : invalid) {
        const auto result = dk::normalize_quaternion(value);
        REQUIRE_FALSE(result.has_value());
        REQUIRE(result.error().code == dk::ErrorCode::invalid_argument);
    }
}

TEMPLATE_TEST_CASE("axis angle rotates actively and composes right to left", "[math]", float, double)
{
    using S = TestType;
    using V = Vector3<S>;
    const auto around_z = dk::rotation_from_axis_angle(V{S{0}, S{0}, S{7}}, dk::radians(S{90}));
    const auto around_x = dk::rotation_from_axis_angle(V{S{2}, S{0}, S{0}}, dk::radians(S{90}));
    REQUIRE(around_z.has_value());
    REQUIRE(around_x.has_value());
    const V x = V::UnitX();
    const V y = V::UnitY();
    const V z = V::UnitZ();
    REQUIRE((*around_z * x).isApprox(y, tolerance<S>));
    const Quaternion<S> composed = *around_x * *around_z;
    REQUIRE((composed * x).isApprox(z, tolerance<S>));
    REQUIRE(std::abs(composed.norm() - S{1}) <= tolerance<S>);

    const auto identity = dk::rotation_from_axis_angle(y, S{0});
    REQUIRE(identity.has_value());
    REQUIRE(identity->toRotationMatrix().isIdentity(tolerance<S>));
    const auto backwards = dk::rotation_from_axis_angle(z, dk::radians(S{-90}));
    REQUIRE(backwards.has_value());
    REQUIRE((*backwards * x).isApprox(-y, tolerance<S>));
    const auto large = dk::rotation_from_axis_angle(z, std::numeric_limits<S>::max());
    REQUIRE(large.has_value());
    REQUIRE(large->coeffs().allFinite());
    REQUIRE(std::abs(large->norm() - S{1}) <= tolerance<S>);
}

TEMPLATE_TEST_CASE("axis angle rejects invalid axis or angle", "[math]", float, double)
{
    using S = TestType;
    using V = Vector3<S>;
    const V zero = V::Zero();
    const V up = V::UnitY();
    const auto no_axis = dk::rotation_from_axis_angle(zero, S{0});
    REQUIRE_FALSE(no_axis.has_value());
    REQUIRE(no_axis.error().code == dk::ErrorCode::invalid_argument);
    const V invalid_axis{S{1}, std::numeric_limits<S>::quiet_NaN(), S{0}};
    REQUIRE_FALSE(dk::rotation_from_axis_angle(invalid_axis, S{1}).has_value());
    for (const S angle : {std::numeric_limits<S>::infinity(), std::numeric_limits<S>::quiet_NaN()}) {
        const auto result = dk::rotation_from_axis_angle(up, angle);
        REQUIRE_FALSE(result.has_value());
        REQUIRE(result.error().code == dk::ErrorCode::invalid_argument);
    }
}

TEMPLATE_TEST_CASE("eigen values are owned and usable in standard containers", "[math]", float, double)
{
    using S = TestType;
    using M = Matrix4<S>;
    std::vector<M> matrices;
    for (int index = 0; index < 32; ++index) {
        M value = M::Identity();
        value(0, 3) = static_cast<S>(index);
        matrices.push_back(value);
    }
    for (const auto& value : matrices) {
        REQUIRE(reinterpret_cast<std::uintptr_t>(&value) % alignof(M) == 0);
    }
    const M snapshot = matrices[1] * matrices[2];
    matrices[1].setZero();
    REQUIRE(snapshot(0, 3) == S{3});
    REQUIRE(snapshot(3, 3) == S{1});
    std::vector<Quaternion<S>> rotations(8, Quaternion<S>::Identity());
    for (const auto& rotation : rotations) {
        REQUIRE(rotation.toRotationMatrix().isIdentity(tolerance<S>));
    }
}

#include <dk/math/Transform.hpp>
#include <dk/math/Math.hpp>

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <limits>
#include <type_traits>
#include <utility>

namespace {

template <typename S>
using V = Eigen::Matrix<S, 3, 1>;
template <typename S>
using M = Eigen::Matrix<S, 4, 4, Eigen::ColMajor>;
template <typename S>
using T = dk::BasicTransform<S>;
template <typename S>
using Trs = dk::BasicTrs<S>;
template <typename S>
constexpr S tolerance = S{256} * std::numeric_limits<S>::epsilon();

template <typename Value>
void require_error(const dk::Result<Value>& result, dk::ErrorCode code)
{
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().code == code);
    REQUIRE_FALSE(result.error().message.empty());
    REQUIRE_FALSE(result.error().context.empty());
}

template <typename Left, typename Right>
concept CanCompose = requires(Left left, Right right) { left.compose(right); };

static_assert(std::is_same_v<dk::Transform, dk::Transformf>);
static_assert(std::is_same_v<dk::Trs, dk::Trsf>);
static_assert(!dk::TransformScalar<long double>);
static_assert(!CanCompose<dk::Transformf, dk::Transformd>);
static_assert(std::is_same_v<decltype(std::declval<const dk::Transform&>().matrix()), const dk::Mat4f&>);

} // namespace

TEMPLATE_TEST_CASE("transform defaults are identity values", "[transform]", float, double)
{
    using S = TestType;
    const Trs<S> trs;
    REQUIRE(trs.translation.isZero());
    REQUIRE(trs.rotation.coeffs() == Eigen::Quaternion<S>::Identity().coeffs());
    REQUIRE(trs.scale == V<S>::Ones());
    const T<S> identity;
    const auto from_trs = T<S>::from_trs(trs);
    REQUIRE(from_trs.has_value());
    REQUIRE(identity.matrix().isIdentity());
    REQUIRE(from_trs->matrix().isIdentity());
    const V<S> input{S{2}, S{-3}, S{4}};
    const auto point = identity.transform_point(input);
    const auto direction = identity.transform_direction(input);
    const auto inverse = identity.inverse();
    REQUIRE(point.has_value());
    REQUIRE(direction.has_value());
    REQUIRE(inverse.has_value());
    REQUIRE(*point == input);
    REQUIRE(*direction == input);
    REQUIRE(inverse->matrix().isIdentity());
}

TEMPLATE_TEST_CASE("trs applies scale then rotation then translation", "[transform]", float, double)
{
    using S = TestType;
    Trs<S> trs;
    trs.translation = V<S>{S{10}, S{20}, S{30}};
    trs.scale = V<S>{S{2}, S{3}, S{4}};
    const auto rotation = dk::rotation_from_axis_angle(V<S>{S{0}, S{0}, S{1}}, dk::radians(S{90}));
    REQUIRE(rotation.has_value());
    trs.rotation = *rotation;
    trs.rotation.coeffs() *= S{7}; // Non-unit input must be normalized without mutating it.
    const auto saved_rotation = trs.rotation;
    const auto transform = T<S>::from_trs(trs);
    REQUIRE(transform.has_value());
    const V<S> input{S{1}, S{2}, S{3}};
    const auto point = transform->transform_point(input);
    const auto direction = transform->transform_direction(input);
    REQUIRE(point.has_value());
    REQUIRE(direction.has_value());
    REQUIRE(point->isApprox(V<S>{S{4}, S{22}, S{42}}, tolerance<S>));
    REQUIRE(direction->isApprox(V<S>{S{-6}, S{2}, S{12}}, tolerance<S>));
    REQUIRE(trs.rotation.coeffs() == saved_rotation.coeffs());
    trs.rotation.coeffs() *= S{-1};
    const auto opposite = T<S>::from_trs(trs);
    REQUIRE(opposite.has_value());
    REQUIRE(opposite->matrix().isApprox(transform->matrix(), tolerance<S>));
}

TEMPLATE_TEST_CASE("parent composition preserves shear from nonuniform scale", "[transform]", float, double)
{
    using S = TestType;
    Trs<S> parent_trs;
    parent_trs.scale = V<S>{S{2}, S{1}, S{1}};
    Trs<S> local_trs;
    const auto rotation = dk::rotation_from_axis_angle(V<S>{S{0}, S{0}, S{1}}, dk::radians(S{45}));
    REQUIRE(rotation.has_value());
    local_trs.rotation = *rotation;
    const auto parent = T<S>::from_trs(parent_trs);
    const auto local = T<S>::from_trs(local_trs);
    REQUIRE(parent.has_value());
    REQUIRE(local.has_value());
    const auto world = parent->compose(*local);
    REQUIRE(world.has_value());
    const auto point = world->transform_point(V<S>{S{1}, S{0}, S{0}});
    REQUIRE(point.has_value());
    REQUIRE(point->isApprox(V<S>{std::sqrt(S{2}), S{1} / std::sqrt(S{2}), S{0}}, tolerance<S>));
    REQUIRE(std::abs(world->matrix().col(0).dot(world->matrix().col(1))) > S{1});
    const auto reversed = local->compose(*parent);
    REQUIRE(reversed.has_value());
    REQUIRE_FALSE(reversed->matrix().isApprox(world->matrix(), tolerance<S>));
    const auto inverse = world->inverse();
    REQUIRE(inverse.has_value());
    const auto restored = inverse->transform_point(*point);
    REQUIRE(restored.has_value());
    REQUIRE(restored->isApprox(V<S>{S{1}, S{0}, S{0}}, tolerance<S>));
}

TEMPLATE_TEST_CASE("three level composition agrees with sequential application", "[transform]", float, double)
{
    using S = TestType;
    Trs<S> root_trs;
    root_trs.translation = V<S>{S{5}, S{-2}, S{1}};
    root_trs.scale = V<S>{S{2}, S{3}, S{4}};
    Trs<S> parent_trs;
    parent_trs.translation = V<S>{S{-3}, S{1}, S{2}};
    const auto rotation = dk::rotation_from_axis_angle(V<S>{S{1}, S{2}, S{3}}, dk::radians(S{37}));
    REQUIRE(rotation.has_value());
    parent_trs.rotation = *rotation;
    Trs<S> local_trs;
    local_trs.scale = V<S>{S{1}, S{-2}, S{3}};
    local_trs.translation = V<S>{S{2}, S{0}, S{-1}};
    const auto root = T<S>::from_trs(root_trs);
    const auto parent = T<S>::from_trs(parent_trs);
    const auto local = T<S>::from_trs(local_trs);
    REQUIRE(root.has_value());
    REQUIRE(parent.has_value());
    REQUIRE(local.has_value());
    const auto root_parent = root->compose(*parent);
    const auto parent_local = parent->compose(*local);
    REQUIRE(root_parent.has_value());
    REQUIRE(parent_local.has_value());
    const auto left = root_parent->compose(*local);
    const auto right = root->compose(*parent_local);
    REQUIRE(left.has_value());
    REQUIRE(right.has_value());
    REQUIRE(left->matrix().isApprox(right->matrix(), tolerance<S>));

    const V<S> input{S{2}, S{4}, S{-1}};
    const auto p1 = local->transform_point(input);
    REQUIRE(p1.has_value());
    const auto p2 = parent->transform_point(*p1);
    REQUIRE(p2.has_value());
    const auto p3 = root->transform_point(*p2);
    const auto combined = left->transform_point(input);
    REQUIRE(p3.has_value());
    REQUIRE(combined.has_value());
    REQUIRE(combined->isApprox(*p3, tolerance<S>));
    const auto identity_composed = T<S>{}.compose(*left);
    REQUIRE(identity_composed.has_value());
    REQUIRE(identity_composed->matrix() == left->matrix());
}

TEMPLATE_TEST_CASE("affine matrix import owns shear and inverse preserves it", "[transform]", float, double)
{
    using S = TestType;
    M<S> matrix = M<S>::Identity();
    matrix << S{2}, S{1}, S{0}, S{7},
              S{0}, S{3}, S{0.5}, S{-2},
              S{0.25}, S{0}, S{4}, S{5},
              S{0}, S{0}, S{0}, S{1};
    const M<S> saved = matrix;
    const auto transform = T<S>::from_matrix(matrix);
    REQUIRE(transform.has_value());
    matrix.setZero(); // Imported values do not alias the caller's matrix.
    REQUIRE(transform->matrix() == saved);
    const auto inverse = transform->inverse();
    REQUIRE(inverse.has_value());
    REQUIRE((transform->matrix() * inverse->matrix()).isIdentity(tolerance<S>));
    REQUIRE((inverse->matrix() * transform->matrix()).isIdentity(tolerance<S>));
    const V<S> input{S{2}, S{-3}, S{4}};
    const auto point = transform->transform_point(input);
    const auto direction = transform->transform_direction(input);
    REQUIRE(point.has_value());
    REQUIRE(direction.has_value());
    const auto restored_point = inverse->transform_point(*point);
    const auto restored_direction = inverse->transform_direction(*direction);
    REQUIRE(restored_point.has_value());
    REQUIRE(restored_direction.has_value());
    REQUIRE(restored_point->isApprox(input, tolerance<S>));
    REQUIRE(restored_direction->isApprox(input, tolerance<S>));
    REQUIRE(transform->matrix() == saved);
}

TEMPLATE_TEST_CASE("negative scale preserves reflection and remains invertible", "[transform]", float, double)
{
    using S = TestType;
    Trs<S> trs;
    trs.scale = V<S>{S{-2}, S{3}, S{4}};
    trs.translation = V<S>{S{5}, S{6}, S{7}};
    const auto transform = T<S>::from_trs(trs);
    REQUIRE(transform.has_value());
    const auto point = transform->transform_point(V<S>{S{1}, S{2}, S{3}});
    REQUIRE(point.has_value());
    REQUIRE(point->isApprox(V<S>{S{3}, S{12}, S{19}}, tolerance<S>));
    const auto inverse = transform->inverse();
    REQUIRE(inverse.has_value());
    const auto restored = inverse->transform_point(*point);
    REQUIRE(restored.has_value());
    REQUIRE(restored->isApprox(V<S>{S{1}, S{2}, S{3}}, tolerance<S>));
    REQUIRE(inverse->matrix()(0, 0) < S{0});
}

TEMPLATE_TEST_CASE("zero scale permits forward transforms but rejects inverse", "[transform]", float, double)
{
    using S = TestType;
    Trs<S> trs;
    trs.translation = V<S>{S{7}, S{8}, S{9}};
    trs.scale = V<S>{S{1}, S{0}, S{2}};
    const auto flat = T<S>::from_trs(trs);
    REQUIRE(flat.has_value());
    const auto point = flat->transform_point(V<S>{S{3}, S{4}, S{5}});
    REQUIRE(point.has_value());
    REQUIRE(*point == V<S>{S{10}, S{8}, S{19}});
    require_error(flat->inverse(), dk::ErrorCode::invalid_state);
    const auto zero_direction = flat->transform_direction(V<S>{S{0}, S{1}, S{0}});
    REQUIRE(zero_direction.has_value());
    REQUIRE(zero_direction->isZero());
    REQUIRE(flat->compose(*flat).has_value());
    trs.scale.setZero();
    const auto collapsed = T<S>::from_trs(trs);
    REQUIRE(collapsed.has_value());
    const auto collapsed_point = collapsed->transform_point(V<S>{S{1}, S{2}, S{3}});
    REQUIRE(collapsed_point.has_value());
    REQUIRE(*collapsed_point == trs.translation);
    require_error(collapsed->inverse(), dk::ErrorCode::invalid_state);
}

TEMPLATE_TEST_CASE("inverse uses relative rank rather than absolute determinant", "[transform]", float, double)
{
    using S = TestType;
    for (const S scale : std::array<S, 4>{S{1e-12}, S{1e12},
             std::numeric_limits<S>::min(), std::numeric_limits<S>::max() / S{16}}) {
        Trs<S> trs;
        trs.scale.setConstant(scale);
        const auto transform = T<S>::from_trs(trs);
        REQUIRE(transform.has_value());
        const auto inverse = transform->inverse();
        REQUIRE(inverse.has_value());
        REQUIRE((transform->matrix() * inverse->matrix()).isIdentity(tolerance<S>));
        REQUIRE((inverse->matrix() * transform->matrix()).isIdentity(tolerance<S>));
    }
    Trs<S> ill_conditioned;
    ill_conditioned.scale[2] = S{32} * std::numeric_limits<S>::epsilon();
    const auto rejected = T<S>::from_trs(ill_conditioned);
    REQUIRE(rejected.has_value());
    require_error(rejected->inverse(), dk::ErrorCode::invalid_state);
    ill_conditioned.scale[2] = S{128} * std::numeric_limits<S>::epsilon();
    const auto accepted = T<S>::from_trs(ill_conditioned);
    REQUIRE(accepted.has_value());
    REQUIRE(accepted->inverse().has_value());

    M<S> dependent = M<S>::Identity();
    dependent.template block<3, 1>(0, 1) = dependent.template block<3, 1>(0, 0);
    const auto singular = T<S>::from_matrix(dependent);
    REQUIRE(singular.has_value());
    require_error(singular->inverse(), dk::ErrorCode::invalid_state);
}

TEMPLATE_TEST_CASE("trs validation rejects nonfinite values and zero rotation", "[transform]", float, double)
{
    using S = TestType;
    for (const S invalid : {std::numeric_limits<S>::quiet_NaN(), std::numeric_limits<S>::infinity()}) {
        Trs<S> bad_translation;
        bad_translation.translation[1] = invalid;
        require_error(T<S>::from_trs(bad_translation), dk::ErrorCode::invalid_argument);
        Trs<S> bad_scale;
        bad_scale.scale[2] = invalid;
        require_error(T<S>::from_trs(bad_scale), dk::ErrorCode::invalid_argument);
        Trs<S> bad_rotation;
        bad_rotation.rotation.x() = invalid;
        require_error(T<S>::from_trs(bad_rotation), dk::ErrorCode::invalid_argument);
    }
    Trs<S> zero_rotation;
    zero_rotation.rotation.coeffs().setZero();
    require_error(T<S>::from_trs(zero_rotation), dk::ErrorCode::invalid_argument);
}

TEMPLATE_TEST_CASE("matrix import rejects projective and nonfinite inputs", "[transform]", float, double)
{
    using S = TestType;
    M<S> input = M<S>::Identity();
    for (Eigen::Index index = 0; index < 3; ++index) {
        input = M<S>::Identity();
        input(3, index) = std::numeric_limits<S>::epsilon();
        require_error(T<S>::from_matrix(input), dk::ErrorCode::invalid_argument);
    }
    input = M<S>::Identity();
    input(3, 3) = S{2};
    require_error(T<S>::from_matrix(input), dk::ErrorCode::invalid_argument);
    for (const S invalid : {std::numeric_limits<S>::quiet_NaN(), std::numeric_limits<S>::infinity()}) {
        input = M<S>::Identity();
        input(0, 3) = invalid;
        require_error(T<S>::from_matrix(input), dk::ErrorCode::invalid_argument);
    }
}

TEMPLATE_TEST_CASE("point and direction reject nonfinite input and range overflow", "[transform]", float, double)
{
    using S = TestType;
    const T<S> identity;
    for (const S invalid : {std::numeric_limits<S>::quiet_NaN(), std::numeric_limits<S>::infinity()}) {
        const V<S> value{S{0}, invalid, S{1}};
        require_error(identity.transform_point(value), dk::ErrorCode::invalid_argument);
        require_error(identity.transform_direction(value), dk::ErrorCode::invalid_argument);
    }
    Trs<S> trs;
    trs.scale[0] = std::numeric_limits<S>::max();
    const auto transform = T<S>::from_trs(trs);
    REQUIRE(transform.has_value());
    const M<S> saved = transform->matrix();
    const V<S> too_large{S{2}, S{0}, S{0}};
    require_error(transform->transform_point(too_large), dk::ErrorCode::invalid_state);
    require_error(transform->transform_direction(too_large), dk::ErrorCode::invalid_state);
    REQUIRE(transform->matrix() == saved);
    trs = Trs<S>{};
    trs.translation[0] = std::numeric_limits<S>::max();
    const auto translated = T<S>::from_trs(trs);
    REQUIRE(translated.has_value());
    const V<S> point{std::numeric_limits<S>::max(), S{0}, S{0}};
    require_error(translated->transform_point(point), dk::ErrorCode::invalid_state);
    REQUIRE(translated->transform_direction(point).has_value());
}

TEMPLATE_TEST_CASE("composition and inverse report unrepresentable results without mutation", "[transform]", float, double)
{
    using S = TestType;
    Trs<S> huge;
    huge.translation[0] = std::numeric_limits<S>::max();
    const auto translated = T<S>::from_trs(huge);
    REQUIRE(translated.has_value());
    const M<S> saved = translated->matrix();
    require_error(translated->compose(*translated), dk::ErrorCode::invalid_state);
    REQUIRE(translated->matrix() == saved);

    huge = Trs<S>{};
    huge.scale[0] = std::numeric_limits<S>::max();
    const auto scaled = T<S>::from_trs(huge);
    REQUIRE(scaled.has_value());
    require_error(scaled->compose(*scaled), dk::ErrorCode::invalid_state);

    Trs<S> tiny;
    tiny.scale.setConstant(std::numeric_limits<S>::denorm_min());
    const auto tiny_transform = T<S>::from_trs(tiny);
    REQUIRE(tiny_transform.has_value());
    require_error(tiny_transform->inverse(), dk::ErrorCode::invalid_state);

    Trs<S> inverse_translation;
    inverse_translation.scale.setConstant(S{0.5});
    inverse_translation.translation[0] = std::numeric_limits<S>::max();
    const auto translation_overflow = T<S>::from_trs(inverse_translation);
    REQUIRE(translation_overflow.has_value());
    require_error(translation_overflow->inverse(), dk::ErrorCode::invalid_state);
}

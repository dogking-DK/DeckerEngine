#include <dk/physics/Xpbd.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <cmath>
#include <limits>
#include <set>

using namespace dk;
using Catch::Approx;
namespace {
XpbdSolver make(XpbdConfig config, std::vector<ParticlePosition> p, std::vector<ParticleVelocity> v,
    std::vector<DistanceConstraint> edges = {}) {
    auto result = XpbdSolver::create(config, std::move(p), std::move(v), std::move(edges));
    if (!result) FAIL(result.error().message);
    return std::move(*result);
}
void same_state(const XpbdSnapshot& a, const XpbdSnapshot& b) {
    REQUIRE(a.positions == b.positions); REQUIRE(a.velocities == b.velocities);
    REQUIRE(a.constraints == b.constraints); REQUIRE(a.color_offsets == b.color_offsets);
    REQUIRE(a.metrics == b.metrics);
}
}
TEST_CASE("XPBD external measurements preserve topology and original pins") {
    auto solver = make({1, -10, -100, 0}, {{0, 1, 0, 0}, {2, 1, 0, 1}}, {{}, {}}, {{0, 1, 2, .001f}});
    auto data = solver.snapshot();
    data.positions[0].y += 1; data.positions[1].x += 1; data.velocities[1].x = 2;
    const auto m = solver.evaluate(data.positions,data.velocities); REQUIRE(m);
    REQUIRE(m->max_pin_displacement == Approx(1)); REQUIRE(m->kinetic_energy == Approx(2));
    REQUIRE(m->max_constraint_error == Approx(std::sqrt(10.0)-2));
    REQUIRE(solver.metrics().max_pin_displacement == 0);
    REQUIRE_FALSE(solver.evaluate({},data.velocities));
    data.positions[0].inverse_mass = 1; REQUIRE_FALSE(solver.evaluate(data.positions,data.velocities));
    data.positions[0].inverse_mass = 0; data.velocities[1].x = std::numeric_limits<float>::infinity();
    REQUIRE_FALSE(solver.evaluate(data.positions,data.velocities));
}
TEST_CASE("XPBD free fall matches the discrete analytic solution and energy") {
    auto solver = make({1, -10, -100, 0}, {{0, 10, 0, 0.5f}}, {{1, 0, 0, 0}});
    REQUIRE(solver.advance(10000000, 20));
    const auto p = solver.positions()[0]; const auto v = solver.velocities()[0];
    REQUIRE(p.x == Approx(0.2).margin(1e-5));
    REQUIRE(p.y == Approx(10 - 10*0.01*0.01*20*21/2).margin(1e-4));
    REQUIRE(v.y == Approx(-2).margin(0.001));
    REQUIRE(solver.metrics().kinetic_energy == Approx(5).margin(0.01));
    REQUIRE(solver.metrics().gravity_potential_energy == Approx(20*(p.y+100)).margin(1e-4));
}
TEST_CASE("XPBD distance solve matches compliance and inverse mass weighted analytic corrections") {
    for (const auto iterations : {1u, 12u}) {
        auto solver = make({iterations, 0, -10, 0}, {{0, 0, 0, 1}, {2, 0, 0, 3}}, {{}, {}}, {{0, 1, 1, 0.0001f}});
        REQUIRE(solver.advance(10000000)); // alpha/dt^2 = 1, delta lambda = -1/(1+3+1)
        REQUIRE(solver.positions()[0].x == Approx(0.2).margin(1e-6));
        REQUIRE(solver.positions()[1].x == Approx(1.4).margin(1e-6));
        REQUIRE(solver.metrics().max_constraint_error == Approx(0.2).margin(1e-6));
        REQUIRE(solver.metrics().compliant_energy == Approx(200).margin(0.002));
    }
    auto hard = make({1, 0, -10, 0}, {{0, 0, 0, 0}, {2, 0, 0, 1}}, {{}, {}}, {{0, 1, 1, 0}});
    REQUIRE(hard.advance(10000000)); REQUIRE(hard.positions()[0].x == 0);
    REQUIRE(hard.positions()[1].x == Approx(1));
    REQUIRE(hard.metrics().max_pin_displacement == 0);
}
TEST_CASE("XPBD contact preserves tangent velocity and removes penetration and downward velocity") {
    auto solver = make({4, -10, 0, 0}, {{0, 0.01f, 0, 1}}, {{2, -10, 0, 0}});
    REQUIRE(solver.advance(10000000));
    REQUIRE(solver.positions()[0].y == 0); REQUIRE(solver.velocities()[0].y == 0);
    REQUIRE(solver.velocities()[0].x == Approx(2));
    REQUIRE(solver.metrics().max_penetration == 0);
    REQUIRE(solver.advance(10000000, 20));
    REQUIRE(solver.positions()[0].y == 0); REQUIRE(solver.velocities()[0].y == 0);
    auto coincident = make({1, 0, -10, 0}, {{0, 0, 0, 1}, {1, 0, 0, 1}}, {{50, 0, 0, 0}, {-50, 0, 0, 0}}, {{0, 1, 1, 0}});
    REQUIRE(coincident.advance(10000000));
    REQUIRE(coincident.positions()[0].x == Approx(0).margin(1e-6));
    REQUIRE(coincident.positions()[1].x == Approx(1).margin(1e-6));
}
TEST_CASE("XPBD rejects malformed topology data and capacity before construction") {
    const std::vector<ParticlePosition> p{{0, 1, 0, 1}, {1, 1, 0, 1}};
    const std::vector<ParticleVelocity> v(2);
    REQUIRE_FALSE(XpbdSolver::create({}, {}, {}, {}));
    REQUIRE_FALSE(XpbdSolver::create({}, p, {}, {}));
    REQUIRE_FALSE(XpbdSolver::create({0, 0, 0, 0}, p, v, {}));
    REQUIRE_FALSE(XpbdSolver::create({}, p, v, {{0, 2, 1, 0}}));
    REQUIRE_FALSE(XpbdSolver::create({}, p, v, {{0, 0, 1, 0}}));
    REQUIRE_FALSE(XpbdSolver::create({}, p, v, {{0, 1, 0, 0}}));
    REQUIRE_FALSE(XpbdSolver::create({}, p, v, {{0, 1, 1, -1}}));
    REQUIRE_FALSE(XpbdSolver::create({}, p, v, {{0, 1, 1, 0}, {1, 0, 1, 0}}));
    auto bad = p; bad[1] = bad[0]; REQUIRE_FALSE(XpbdSolver::create({}, bad, v, {{0, 1, 1, 0}}));
    bad = p; bad[0].x = std::numeric_limits<float>::quiet_NaN(); REQUIRE_FALSE(XpbdSolver::create({}, bad, v, {}));
    bad = p; bad[0].inverse_mass = -1; REQUIRE_FALSE(XpbdSolver::create({}, bad, v, {}));
    bad = p; bad[0].inverse_mass = 0; bad[0].y = -1; REQUIRE_FALSE(XpbdSolver::create({}, bad, v, {}));
    auto moving_pin = v; moving_pin[0].x = 1; bad[0].y = 1;
    REQUIRE_FALSE(XpbdSolver::create({}, bad, moving_pin, {}));
    REQUIRE_FALSE(XpbdSolver::create({}, std::vector<ParticlePosition>(1025), std::vector<ParticleVelocity>(1025), {}));
    REQUIRE_FALSE(XpbdSolver::create({}, p, v, std::vector<DistanceConstraint>(8193)));
    std::vector<ParticlePosition> star; std::vector<DistanceConstraint> edges;
    for (std::uint32_t i = 0; i < 34; ++i) { star.push_back({static_cast<float>(i), 1, 0, 1}); if (i) edges.push_back({0, i, static_cast<float>(i), 0}); }
    REQUIRE_FALSE(XpbdSolver::create({}, star, std::vector<ParticleVelocity>(34), edges));
}
TEST_CASE("XPBD rejected budgets and mid batch numerical failure preserve prior arrays and metrics") {
    auto result = XpbdSolver::cloth(); REQUIRE(result); auto solver = std::move(*result);
    const auto before = solver.snapshot();
    REQUIRE_FALSE(solver.advance(0)); REQUIRE_FALSE(solver.advance(1000000000));
    REQUIRE_FALSE(solver.advance(10000000, 0)); REQUIRE_FALSE(solver.advance(10000000, 10001));
    REQUIRE_FALSE(solver.advance(10000000, 10000)); same_state(solver.snapshot(), before);
    auto divergent = make({1, 0, -10, 0}, {{999990, 1, 0, 1}}, {{100, 0, 0, 0}});
    auto copy = divergent;
    REQUIRE(copy.advance(10000000)); // first step succeeds, failure is later in the batch
    const auto original = divergent.snapshot();
    const auto failure = divergent.advance(10000000, 20);
    REQUIRE_FALSE(failure); REQUIRE(failure.error().code == ErrorCode::invalid_state);
    same_state(divergent.snapshot(), original);
}
TEST_CASE("XPBD seeded cloth has disjoint color groups and reproducible bounded 300 step dynamics") {
    auto created = XpbdSolver::cloth(); REQUIRE(created);
    auto all = *created, split = *created;
    ClothConfig other; other.seed = 2; auto different = XpbdSolver::cloth(other); REQUIRE(different);
    REQUIRE(different->snapshot().positions != all.snapshot().positions);
    const auto initial = all.snapshot();
    REQUIRE(initial.positions.size() == 64); REQUIRE(initial.constraints.size() == 210);
    for (std::size_t i = 1; i < initial.color_offsets.size(); ++i) {
        std::set<std::uint32_t> used;
        for (auto j = initial.color_offsets[i-1]; j < initial.color_offsets[i]; ++j) {
            REQUIRE(used.insert(initial.constraints[j].a).second); REQUIRE(used.insert(initial.constraints[j].b).second);
        }
    }
    REQUIRE(all.advance(10000000, 300));
    for (int i = 0; i < 3; ++i) REQUIRE(split.advance(10000000, 100));
    same_state(all.snapshot(), split.snapshot());
    const auto final = all.snapshot();
    for (std::size_t i = 0; i < 8; ++i) REQUIRE(final.positions[i] == initial.positions[i]);
    REQUIRE(final.positions[63].y < initial.positions[63].y - 0.1f);
    REQUIRE(final.metrics.max_pin_displacement == 0); REQUIRE(final.metrics.max_penetration <= 1e-6);
    REQUIRE(final.metrics.max_relative_error < 0.08);
    REQUIRE(std::isfinite(final.metrics.kinetic_energy));
    double error2 = 0, maximum = 0, energy = 0;
    for (const auto& c : final.constraints) {
        const auto a = final.positions[c.a], b = final.positions[c.b];
        const double error = std::hypot(static_cast<double>(a.x)-b.x, static_cast<double>(a.y)-b.y, static_cast<double>(a.z)-b.z)-c.rest_length;
        error2 += error*error; maximum = std::max(maximum, std::abs(error));
    }
    for (std::size_t i = 0; i < final.positions.size(); ++i) if (final.positions[i].inverse_mass) {
        const auto v = final.velocities[i];
        energy += 0.5*(static_cast<double>(v.x)*v.x + static_cast<double>(v.y)*v.y + static_cast<double>(v.z)*v.z)/final.positions[i].inverse_mass;
    }
    REQUIRE(final.metrics.rms_constraint_error == Approx(std::sqrt(error2/final.constraints.size())).margin(1e-12));
    REQUIRE(final.metrics.max_constraint_error == Approx(maximum).margin(1e-12));
    REQUIRE(final.metrics.kinetic_energy == Approx(energy).margin(1e-12));
}

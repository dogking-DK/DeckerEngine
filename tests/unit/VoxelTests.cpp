#include <dk/geometry/Voxels.hpp>
#include "VoxelOracle.hpp"
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <random>

using namespace dk;
using namespace dk::geometry;
namespace vx = dk::geometry::voxels;

TEST_CASE("dense input validation and owning failure contract", "[voxels]") {
    std::vector<std::uint32_t> cells{1, 0, 2, 1};
    vx::DenseInput input{{-2, 0, 0}, {2, 2, 1}, cells, 2};
    auto grid = vx::DenseGrid::build(input);
    REQUIRE(grid);
    REQUIRE(grid->value({-2, 0, 0}) == 1);
    REQUIRE(grid->value({-1, 0, 0}) == 0);
    REQUIRE(grid->value({-2, 1, 0}) == 2);
    REQUIRE(grid->value({INT32_MIN, 0, 0}) == 0);
    cells[0] = 99;
    REQUIRE(grid->value({-2, 0, 0}) == 1);
    REQUIRE_FALSE(vx::DenseGrid::build(input));
    cells[0] = 1;
    REQUIRE_FALSE(vx::DenseGrid::build(input, {3}));
    auto bad = input; bad.dimensions[0] = 0;
    REQUIRE_FALSE(vx::DenseGrid::build(bad));
    bad.dimensions = {UINT32_MAX, UINT32_MAX, UINT32_MAX};
    REQUIRE_FALSE(vx::DenseGrid::build(bad, {UINT64_MAX}));
    bad = input; bad.values = std::span(cells).first(3);
    REQUIRE_FALSE(vx::DenseGrid::build(bad));
    bad = input; bad.minimum[0] = INT32_MAX;
    REQUIRE_FALSE(vx::DenseGrid::build(bad));
    bad = input; bad.minimum[0] = INT32_MIN;
    REQUIRE_FALSE(vx::DenseGrid::build(bad));
    bad = input; bad.palette_size = 0;
    REQUIRE_FALSE(vx::DenseGrid::build(bad));
    Trsd trs; trs.scale.x() = 0;
    bad = input; bad.corner_to_local = Transformd::from_trs(trs).value();
    REQUIRE_FALSE(vx::DenseGrid::build(bad));
    REQUIRE(grid->value({-2, 0, 0}) == 1);
}

TEST_CASE("dense surfaces handle entry exit clipping ties and grazing", "[voxels]") {
    std::vector<std::uint32_t> cells{1, 2};
    auto grid = vx::DenseGrid::build({{}, {2, 1, 1}, cells, 2}).value();
    const auto entry = grid.nearest({{-1, .5, .5}, {2, 0, 0}});
    REQUIRE(entry); REQUIRE(entry->hit);
    REQUIRE(entry->hit->distance == .5);
    REQUIRE(entry->hit->face == vx::Face::negative_x);
    REQUIRE(entry->hit->cell == vx::Coord{0, 0, 0});
    const auto exit = grid.nearest({{.25, .5, .5}, {1, 0, 0}});
    REQUIRE(exit); REQUIRE(exit->hit);
    REQUIRE(exit->hit->distance == 1.75);
    REQUIRE(exit->hit->value == 2);
    REQUIRE(exit->hit->face == vx::Face::positive_x); // No internal material boundary.
    auto clipped = grid.nearest({{-1, .5, .5}, {1, 0, 0}, 1.25, 5}).value();
    REQUIRE(clipped.hit); REQUIRE(clipped.hit->distance == 3);
    REQUIRE_FALSE(grid.nearest({{-1, .5, .5}, {1, 0, 0}, 0, .99})->hit);
    REQUIRE(grid.nearest({{-1, .5, .5}, {1, 0, 0}, 1, 1})->hit->distance == 1);
    REQUIRE(grid.nearest({{-1, .5, .5}, {1, 0, 0}, 3, 3})->hit->distance == 3);
    REQUIRE_FALSE(grid.nearest({{-1, .5, 1}, {1, 0, 0}})->hit);
    REQUIRE_FALSE(grid.nearest({{-1, 1, .5}, {1, -1, 0}})->hit);
    auto diagonal = grid.nearest({{-1, -1, -1}, {1, 1, 1}}).value();
    REQUIRE(diagonal.hit); REQUIRE(diagonal.hit->distance == 1);
    REQUIRE(diagonal.hit->face == vx::Face::negative_x);
    REQUIRE_FALSE(grid.nearest({{0, 0, 0}, {0, 0, 0}}));
    REQUIRE_FALSE(grid.nearest({{0, 0, 0}, {1, 0, 0}, 2, 1}));
    Ray invalid; invalid.origin.x() = std::numeric_limits<double>::quiet_NaN();
    REQUIRE_FALSE(grid.nearest(invalid));
    invalid = {{1e308, .5, .5}, {1e-308, 0, 0}};
    REQUIRE_FALSE(grid.nearest(invalid));
    cells.assign(5, 0);
    auto empty = vx::DenseGrid::build({{}, {5, 1, 1}, cells, 1}).value();
    REQUIRE_FALSE(empty.nearest({{-1, .5, .5}, {1, 0, 0}}, {}, {1}));
    REQUIRE_FALSE(empty.nearest({{-1, .5, .5}, {1, 0, 0}})->hit);
}

TEST_CASE("dense affine instances preserve ray parameter and inverse transpose normal", "[voxels]") {
    const std::vector<std::uint32_t> cells{1};
    auto grid = vx::DenseGrid::build({{}, {1, 1, 1}, cells, 1}).value();
    Mat4d matrix = Mat4d::Identity();
    matrix(0, 0) = -2; matrix(1, 1) = 3; matrix(2, 2) = .25;
    matrix(0, 1) = .5; matrix(2, 0) = .125;
    matrix(0, 3) = 17;
    auto world = Transformd::from_matrix(matrix).value();
    const Ray local{{-1, .25, .75}, {2, 0, 0}};
    const Ray ray{world.transform_point(local.origin).value(), world.transform_direction(local.direction).value()};
    auto result = grid.nearest(ray, world);
    REQUIRE(result); REQUIRE(result->hit);
    REQUIRE(result->hit->distance == Catch::Approx(.5));
    Vec3d expected = world.inverse()->matrix().block<3, 3>(0, 0).transpose() * -Vec3d::UnitX();
    expected.normalize();
    REQUIRE(result->hit->normal.isApprox(expected, 1e-12));
    Trsd singular; singular.scale.z() = 0;
    REQUIRE_FALSE(grid.nearest(ray, Transformd::from_trs(singular).value()));
}

TEST_CASE("dense external normals ignore internal grid plane ties", "[voxels]") {
    const std::vector<std::uint32_t> cells(64,1);
    const auto grid=vx::DenseGrid::build({{0,0,0},{4,4,4},cells,1}).value();
    const auto entry=grid.nearest({{1,.5,5},{1,0,-1}}).value();
    REQUIRE(entry.hit); CHECK(entry.hit->face==vx::Face::positive_z); CHECK(entry.hit->normal==Vec3d::UnitZ());
    const auto exit=grid.nearest({{1,.5,1},{1,0,-1}}).value();
    REQUIRE(exit.hit); CHECK(exit.hit->face==vx::Face::negative_z); CHECK(exit.hit->normal==-Vec3d::UnitZ());
}
TEST_CASE("dense DDA matches independent exposed face oracle", "[voxels]") {
    std::mt19937 random(2302);
    std::uniform_real_distribution<double> position(-3.7, 6.3), direction(-1, 1);
    for (int fixture = 0; fixture < 6; ++fixture) {
        std::vector<std::uint32_t> cells(64);
        for (int z = 0; z < 4; ++z) for (int y = 0; y < 4; ++y) for (int x = 0; x < 4; ++x) {
            auto& v = cells[x + 4 * (y + 4 * z)];
            if (fixture == 1) v = 1;
            if (fixture == 2) v = x == 0 || x == 3 || y == 0 || y == 3 || z == 0 || z == 3;
            if (fixture == 3) v = ((x + y + z) % 2) ? 2u : 0u;
            if (fixture == 4) v = static_cast<std::uint32_t>(random() % 3);
            if (fixture == 5) v = x == 2 && y == 1 && z == 3;
        }
        auto grid = vx::DenseGrid::build({{-1, -1, -1}, {4, 4, 4}, cells, 2}).value();
        for (int trial = 0; trial < 512; ++trial) {
            Ray ray{{position(random), position(random), position(random)}, {direction(random), direction(random), direction(random)}, trial % 3 == 0 ? .3 : 0, 20};
            const auto expected = voxel_face_oracle(grid, ray);
            const auto actual = grid.nearest(ray);
            INFO("fixture=" << fixture << " trial=" << trial);
            REQUIRE(actual);
            REQUIRE(actual->hit.has_value() == expected.has_value());
            if (expected) {
                REQUIRE(actual->hit->distance == Catch::Approx(expected->distance).margin(1e-10));
                REQUIRE(actual->hit->cell == expected->cell);
                REQUIRE(actual->hit->face == expected->face);
                REQUIRE(actual->hit->value == expected->value);
            }
        }
    }
}

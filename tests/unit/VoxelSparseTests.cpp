#include <dk/assets/VoxelBuilder.hpp>
#include "OpenVdbConversion.hpp"
#include "VoxelOracle.hpp"
#ifdef _MSC_VER
// Same upstream toGridType<T> warning scope as the private runtime adapter.
#pragma warning(push)
#pragma warning(disable: 4702)
#endif
#include <nanovdb/NanoVDB.h>
#ifdef _MSC_VER
#pragma warning(pop)
#endif
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <future>
#include <random>

using namespace dk;
using namespace dk::geometry;
using namespace dk::assets;
namespace vx = dk::geometry::voxels;

namespace {
Transformd centered_cells() {
    Trsd trs; trs.translation = Vec3d::Constant(.5);
    return Transformd::from_trs(trs).value();
}
}

TEST_CASE("CPU sparse values and rays match dense and independent face oracle", "[voxels][vdb]") {
    std::mt19937 random(230202);
    for (int fixture = 0; fixture < 6; ++fixture) {
        std::vector<std::uint32_t> values(64);
        std::vector<VoxelBlock> blocks;
        for (int z = 0; z < 4; ++z) for (int y = 0; y < 4; ++y) for (int x = 0; x < 4; ++x) {
            std::uint32_t value = 0;
            if (fixture == 1) value = 1;
            if (fixture == 2) value = x == 0 || x == 3 || y == 0 || y == 3 || z == 0 || z == 3;
            if (fixture == 3) value = (x + y + z) % 2 ? 2u : 0u;
            if (fixture == 4) value = static_cast<std::uint32_t>(random() % 3);
            if (fixture == 5) value = x == 2 && y == 1 && z == 3;
            values[x + 4 * (y + 4 * z)] = value;
            blocks.push_back({{x - 2, y - 2, z - 2}, 1, static_cast<std::int32_t>(value), random() % 2 != 0});
        }
        VoxelBuildOptions options;
        options.palette_size = 2; options.sample_to_local = centered_cells();
        options.domain = vx::Domain{{-2, -2, -2}, {1, 1, 1}};
        auto sparse = build_voxel_grid(blocks, options);
        REQUIRE(sparse);
        auto dense = vx::DenseGrid::build({{-2, -2, -2}, {4, 4, 4}, values, 2}).value();
        const auto* nano = reinterpret_cast<const nanovdb::NanoGrid<std::int32_t>*>(sparse->bytes().data());
        REQUIRE(reinterpret_cast<std::uintptr_t>(sparse->bytes().data()) % NANOVDB_DATA_ALIGNMENT == 0);
        for (int z = -3; z < 3; ++z) for (int y = -3; y < 3; ++y) for (int x = -3; x < 3; ++x) {
            const vx::Coord p{x, y, z};
            REQUIRE(sparse->value(p) == dense.value(p));
            REQUIRE(nano->tree().isActive(nanovdb::Coord(x, y, z)) == (dense.value(p) != 0));
        }
        std::uniform_real_distribution<double> position(-4.7, 4.3), direction(-1, 1);
        for (int i = 0; i < 1024; ++i) {
            Ray ray{{position(random), position(random), position(random)}, {direction(random), direction(random), direction(random)}, i % 3 ? 0 : .4, 20};
            const auto expected = voxel_face_oracle(dense, ray);
            const auto dense_hit = dense.nearest(ray);
            const auto result = sparse->nearest(ray);
            INFO("fixture=" << fixture << " ray=" << i);
            REQUIRE(result); REQUIRE(dense_hit);
            REQUIRE(result->hit.has_value() == expected.has_value());
            REQUIRE(result->hit.has_value() == dense_hit->hit.has_value());
            if (expected) {
                REQUIRE(result->hit->distance == Catch::Approx(expected->distance).margin(1e-10));
                REQUIRE(result->hit->cell == expected->cell);
                REQUIRE(result->hit->face == expected->face);
                REQUIRE(result->hit->value == expected->value);
            }
        }
    }
}

TEST_CASE("inactive constant tiles normalize without expansion or internal faces", "[voxels][vdb]") {
    const std::array blocks{VoxelBlock{{-4096, 0, 0}, 4096, 1, false}, VoxelBlock{{0, 0, 0}, 4096, 2, true}};
    VoxelBuildOptions options; options.palette_size = 2;
    auto grid = build_voxel_grid(blocks, options);
    REQUIRE(grid);
    REQUIRE(grid->statistics().node_counts == std::array<std::uint32_t, 3>{0, 0, 0});
    REQUIRE(grid->statistics().active_voxels == 2ull * 4096 * 4096 * 4096);
    REQUIRE(grid->bytes().size() < 8192);
    REQUIRE(grid->value({-4096, 0, 0}) == 1);
    REQUIRE(grid->value({4095, 4095, 4095}) == 2);
    REQUIRE(grid->maximum_value() == 2);
    REQUIRE(grid->value({4096, 0, 0}) == 0);
    // Starts inside the first tile; the material/tile boundary at -0.5 is not a surface.
    auto result = grid->nearest({{-1, 0, 0}, {1, 0, 0}});
    REQUIRE(result); REQUIRE(result->hit);
    REQUIRE(result->hit->distance == 4096.5);
    REQUIRE(result->hit->cell == vx::Coord{4095, 0, 0});
    REQUIRE(result->hit->value == 2);
    REQUIRE(result->visited_regions <= 2);
    REQUIRE(grid->nearest({{-4097, 0, 0}, {1, 0, 0}})->hit->distance == .5);
}

TEST_CASE("sparse large coordinate span skips empty hierarchy within work budget", "[voxels][vdb]") {
    const std::array blocks{VoxelBlock{{-1048576, 0, 0}, 1, 1, false}, VoxelBlock{{1048576, 0, 0}, 1, 1, true}};
    auto grid = build_voxel_grid(blocks);
    REQUIRE(grid);
    REQUIRE(grid->bytes().size() < 1024 * 1024);
    REQUIRE(grid->statistics().node_counts[0] == 2);
    const Ray ray{{-1048574, 0, 0}, {1, 0, 0}};
    auto hit = grid->nearest(ray, {}, {1024});
    REQUIRE(hit); REQUIRE(hit->hit);
    REQUIRE(hit->hit->cell == vx::Coord{1048576, 0, 0});
    REQUIRE(hit->hit->distance == 2097149.5);
    REQUIRE(hit->visited_regions < 1024);
    REQUIRE_FALSE(grid->nearest(ray, {}, {1}));
}

TEST_CASE("leaf tile transitions hollow regions and boundaries preserve surface semantics", "[voxels][vdb]") {
    std::vector<VoxelBlock> blocks{{{0, 0, 0}, 8, 1, false}, {{8, 0, 0}, 8, 1, true}, {{7, 3, 3}, 1, 0, true}};
    VoxelBuildOptions options; options.sample_to_local = centered_cells();
    auto grid = build_voxel_grid(blocks, options);
    REQUIRE(grid);
    auto cavity = grid->nearest({{1.5, 3.5, 3.5}, {1, 0, 0}});
    REQUIRE(cavity); REQUIRE(cavity->hit);
    REQUIRE(cavity->hit->distance == 5.5);
    REQUIRE(cavity->hit->cell == vx::Coord{6, 3, 3});
    auto across = grid->nearest({{1.5, 2.5, 2.5}, {1, 0, 0}}).value();
    REQUIRE(across.hit); REQUIRE(across.hit->distance == 14.5);
    REQUIRE(across.hit->cell == vx::Coord{15, 2, 2});
    auto corner = grid->nearest({{-1, -1, -1}, {1, 1, 1}}).value();
    REQUIRE(corner.hit); REQUIRE(corner.hit->face == vx::Face::negative_x);
    REQUIRE(corner.hit->distance == 1);
    const auto planar=grid->nearest({{1,.5,9},{1,0,-1}}).value();
    REQUIRE(planar.hit); REQUIRE(planar.hit->face==vx::Face::positive_z);
    REQUIRE_FALSE(grid->nearest({{-1, 0, 4}, {1, 0, 0}})->hit);
    auto from_hole = grid->nearest({{7.5, 3.5, 3.5}, {1, 0, 0}}).value();
    REQUIRE(from_hole.hit); REQUIRE(from_hole.hit->distance == .5);
    REQUIRE(from_hole.hit->cell == vx::Coord{8, 3, 3});
}

TEST_CASE("grid sample centers and affine instance maps compose exactly once", "[voxels][vdb]") {
    const std::array blocks{VoxelBlock{{-2, 1, 0}, 1, 1, true}};
    VoxelBuildOptions options;
    Mat4d source = Mat4d::Identity();
    source(0, 0) = -2; source(1, 1) = 3; source(2, 2) = .25;
    source(0, 1) = .5; source(2, 0) = .125; source(1, 3) = -17;
    options.sample_to_local = Transformd::from_matrix(source).value();
    auto sparse = build_voxel_grid(blocks, options);
    REQUIRE(sparse);
    Mat4d corner = source;
    corner.block<3, 1>(0, 3) -= .5 * source.block<3, 3>(0, 0) * Vec3d::Ones();
    REQUIRE(sparse->corner_to_local().matrix().isApprox(corner, 1e-12));
    const auto* nano = reinterpret_cast<const nanovdb::NanoGrid<std::int32_t>*>(sparse->bytes().data());
    const auto nano_point = nano->indexToWorld(nanovdb::Vec3d(-2, 1, 0));
    const auto expected_point = options.sample_to_local.transform_point({-2, 1, 0}).value();
    for (int axis = 0; axis < 3; ++axis) REQUIRE(nano_point[axis] == Catch::Approx(expected_point[axis]).margin(1e-12));
    Trsd instance; instance.translation = {10, -4, 2}; instance.scale = {2, .5, -3};
    auto world = Transformd::from_trs(instance).value();
    auto complete = world.compose(sparse->corner_to_local()).value();
    Ray ray{complete.transform_point({-3, 1.25, .75}).value(), complete.transform_direction({2, 0, 0}).value()};
    auto result = sparse->nearest(ray, world);
    REQUIRE(result); REQUIRE(result->hit);
    REQUIRE(result->hit->distance == Catch::Approx(.5).margin(1e-12));
    REQUIRE(result->hit->cell == vx::Coord{-2, 1, 0});
    Vec3d normal = complete.inverse()->matrix().block<3, 3>(0, 0).transpose() * -Vec3d::UnitX();
    normal.normalize();
    REQUIRE(result->hit->normal.isApprox(normal, 1e-12));
}

TEST_CASE("conversion rejects unsupported grids and preserves input active states", "[voxels][vdb]") {
    VoxelBuildOptions options; options.palette_size = 2;
    auto source = openvdb::Int32Grid::create(0);
    source->tree().addTile(1, openvdb::Coord(-8, 0, 0), 2, false);
    source->tree().setValue(openvdb::Coord(0, 0, 0), 0);
    auto result = dk::assets::detail::convert_openvdb(*source, options);
    REQUIRE(result);
    REQUIRE_FALSE(source->tree().isValueOn(openvdb::Coord(-8, 0, 0)));
    REQUIRE(source->tree().isValueOn(openvdb::Coord(0, 0, 0)));
    REQUIRE(result->value({-8, 0, 0}) == 2);
    auto floating = openvdb::FloatGrid::create(0);
    REQUIRE_FALSE(dk::assets::detail::convert_openvdb(*floating, options));
    source->setGridClass(openvdb::GRID_LEVEL_SET);
    REQUIRE_FALSE(dk::assets::detail::convert_openvdb(*source, options));
    source->setGridClass(openvdb::GRID_FOG_VOLUME);
    REQUIRE_FALSE(dk::assets::detail::convert_openvdb(*source, options));
    auto background = openvdb::Int32Grid::create(1);
    REQUIRE_FALSE(dk::assets::detail::convert_openvdb(*background, options));
    source->setGridClass(openvdb::GRID_UNKNOWN);
    source->tree().setValueOff(openvdb::Coord(20, 0, 0), -1);
    REQUIRE_FALSE(dk::assets::detail::convert_openvdb(*source, options));
    source->tree().setValueOff(openvdb::Coord(20, 0, 0), 3);
    REQUIRE_FALSE(dk::assets::detail::convert_openvdb(*source, options));
    source->tree().setValueOff(openvdb::Coord(20, 0, 0), 0);
    source->setTransform(openvdb::math::Transform::createFrustumTransform(
        openvdb::BBoxd(openvdb::Vec3d(0), openvdb::Vec3d(1)), .5, 10, 1));
    REQUIRE_FALSE(dk::assets::detail::convert_openvdb(*source, options));
    REQUIRE(result->value({-8, 0, 0}) == 2);
}

TEST_CASE("conversion budgets invalid inputs and old snapshots survive rejection", "[voxels][vdb]") {
    std::vector<VoxelBlock> blocks{{{0, 0, 0}, 1, 1, true}};
    auto old = build_voxel_grid(blocks);
    REQUIRE(old);
    const auto* previous = old->bytes().data();
    VoxelBuildOptions options;
    options.limits.maximum_nano_bytes = 1;
    REQUIRE_FALSE(build_voxel_grid(blocks, options));
    options = {}; options.limits.maximum_tree_bytes = 1;
    REQUIRE_FALSE(build_voxel_grid(blocks, options));
    options = {}; options.limits.maximum_value_visits = 1;
    REQUIRE_FALSE(build_voxel_grid(blocks, options));
    options = {}; options.limits.maximum_blocks = 0;
    REQUIRE_FALSE(build_voxel_grid(blocks, options));
    options = {}; options.domain = vx::Domain{{1, 1, 1}, {3, 3, 3}};
    REQUIRE_FALSE(build_voxel_grid(blocks, options));
    options = {}; options.palette_size = 0;
    REQUIRE_FALSE(build_voxel_grid(blocks, options));
    options = {}; Trsd singular; singular.scale.x() = 0; options.sample_to_local = Transformd::from_trs(singular).value();
    REQUIRE_FALSE(build_voxel_grid(blocks, options));
    blocks[0].dimension = 7;
    REQUIRE_FALSE(build_voxel_grid(blocks));
    blocks[0] = {{-1, 0, 0}, 8, 1, true};
    REQUIRE_FALSE(build_voxel_grid(blocks));
    blocks[0] = {{INT32_MAX, 0, 0}, 1, 1, true};
    REQUIRE_FALSE(build_voxel_grid(blocks));
    blocks[0] = {{0, 0, 0}, 1, -1, false};
    REQUIRE_FALSE(build_voxel_grid(blocks));
    blocks[0].value = 2;
    REQUIRE_FALSE(build_voxel_grid(blocks));
    REQUIRE(old->bytes().data() == previous);
    REQUIRE(old->value({0, 0, 0}) == 1);
    REQUIRE(old->nearest({{-1, 0, 0}, {1, 0, 0}})->hit->distance == .5);
}

TEST_CASE("empty grids and concurrent snapshots require no mutable tree", "[voxels][vdb]") {
    auto empty = build_voxel_grid({});
    REQUIRE(empty);
    REQUIRE(empty->statistics().active_voxels == 0);
    REQUIRE(empty->maximum_value() == 0);
    REQUIRE_FALSE(empty->nearest({{-1, 0, 0}, {1, 0, 0}})->hit);
    const std::array blocks{VoxelBlock{{0, 0, 0}, 8, 1, false}};
    auto retained = build_voxel_grid(blocks).value();
    auto copy = retained;
    REQUIRE(copy.bytes().data() == retained.bytes().data());
    auto task = [copy] {
        for (int i = 0; i < 100; ++i) {
            const auto hit = copy.nearest({{-1, 0, 0}, {1, 0, 0}});
            if (!hit || !hit->hit || hit->hit->distance != .5) return false;
        }
        return true;
    };
    auto a = std::async(std::launch::async, task), b = std::async(std::launch::async, task);
    retained = build_voxel_grid({}).value();
    REQUIRE(a.get()); REQUIRE(b.get());
    REQUIRE(copy.value({0, 0, 0}) == 1);
    REQUIRE(retained.value({0, 0, 0}) == 0);
}

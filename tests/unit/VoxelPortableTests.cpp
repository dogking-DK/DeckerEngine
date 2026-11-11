#include <dk/assets/VoxelBuilder.hpp>
#define PNANOVDB_C
#define PNANOVDB_ADDRESS_32
#define PNANOVDB_BUF_BOUNDS_CHECK
#include <nanovdb/PNanoVDB.h>
#include <catch2/catch_test_macros.hpp>
#include <cstring>

TEST_CASE("portable CPU accessor agrees on NanoVDB Int32 buffer layout", "[voxels][vdb]") {
    using namespace dk::assets;
    const std::array blocks{VoxelBlock{{-4096, 0, 0}, 4096, 1, false},
        VoxelBlock{{0, 0, 0}, 128, 2, true}, VoxelBlock{{128, 0, 0}, 8, 3, false},
        VoxelBlock{{137, 0, 0}, 1, 4, true}, VoxelBlock{{138, 0, 0}, 1, 0, true},
        VoxelBlock{{139, 0, 0}, 1, INT32_MAX, false}, VoxelBlock{{140, 0, 0}, 1, 16777217, true}};
    VoxelBuildOptions options; options.palette_size = INT32_MAX;
    auto grid = build_voxel_grid(blocks, options);
    REQUIRE(grid);
    // The portable C interface takes a mutable word pointer; copy instead of casting away immutability.
    std::vector<std::uint32_t> words((grid->bytes().size() + 3) / 4);
    std::memcpy(words.data(), grid->bytes().data(), grid->bytes().size());
    auto buffer = pnanovdb_make_buf(words.data(), words.size());
    pnanovdb_grid_handle_t handle{pnanovdb_address_null()}; // Grid starts at byte zero.
    REQUIRE(pnanovdb_grid_get_grid_type(buffer, handle) == PNANOVDB_GRID_TYPE_INT32);
    REQUIRE(pnanovdb_grid_get_grid_size(buffer, handle) == grid->bytes().size());
    auto root = pnanovdb_tree_get_root(buffer, pnanovdb_grid_get_tree(buffer, handle));
    pnanovdb_readaccessor_t accessor;
    pnanovdb_readaccessor_init(&accessor, root);
    for (const int x : {-5000, -4096, -1, 0, 127, 128, 135, 136, 137, 138, 139, 140, 200}) {
        pnanovdb_coord_t coord{x, 0, 0};
        const auto address = pnanovdb_readaccessor_get_value_address(PNANOVDB_GRID_TYPE_INT32, buffer, &accessor, &coord);
        REQUIRE(pnanovdb_read_int32(buffer, address) == static_cast<std::int32_t>(grid->value({x, 0, 0})));
    }
}

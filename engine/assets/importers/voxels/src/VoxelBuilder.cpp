#include <dk/assets/VoxelBuilder.hpp>
#include "OpenVdbConversion.hpp"
#include "NanoStorage.hpp"
#ifdef _MSC_VER
// NanoVDB Invoke/Reduce templates contain unreachable fallback returns as well.
#pragma warning(push)
#pragma warning(disable: 4702)
#endif
#include <nanovdb/tools/CreateNanoGrid.h>
#ifdef _MSC_VER
#pragma warning(pop)
#endif
#include <mutex>
#include <limits>

namespace dk::assets {
namespace {
using namespace geometry::voxels;
Result<void> validate_options(const VoxelBuildOptions& options) {
    if (!options.palette_size || options.palette_size > static_cast<std::uint32_t>(INT32_MAX) ||
        !options.limits.maximum_tree_bytes || !options.limits.maximum_nano_bytes || !options.limits.maximum_value_visits)
        return std::unexpected(Error{ErrorCode::invalid_argument, "Voxel palette and conversion budgets must be positive and representable"});
    if (options.domain) if (auto valid = validate(*options.domain); !valid) return std::unexpected(valid.error());
    if (auto inverse = options.sample_to_local.inverse(); !inverse) return std::unexpected(inverse.error());
    return {};
}
Error conversion_error(const std::exception& exception) {
    return {ErrorCode::invalid_argument, std::string("OpenVDB conversion failed: ") + exception.what()};
}
}

Result<geometry::voxels::SparseGrid> detail::convert_openvdb(const openvdb::GridBase& source, const VoxelBuildOptions& options) {
    using namespace geometry::voxels;
    if (auto valid = validate_options(options); !valid) return std::unexpected(valid.error());
    if (!source.isType<openvdb::Int32Grid>() || source.getGridClass() != openvdb::GRID_UNKNOWN)
        return std::unexpected(Error{ErrorCode::not_supported, "Solid voxels require an explicitly selected unclassified Int32Grid"});
    if (!source.transform().isLinear())
        return std::unexpected(Error{ErrorCode::not_supported, "Nonlinear OpenVDB maps are not supported"});
    const auto& input = static_cast<const openvdb::Int32Grid&>(source);
    if (input.background() != 0)
        return std::unexpected(Error{ErrorCode::invalid_argument, "Solid voxel background must be zero"});
    const auto source_bytes = input.memUsage();
    if (source_bytes > options.limits.maximum_tree_bytes)
        return std::unexpected(Error{ErrorCode::invalid_argument, "OpenVDB source exceeds tree budget"});
    try {
        // OpenVDB uses row-vector maps; derive the engine column-vector map from basis samples.
        const auto origin = input.indexToWorld(openvdb::Vec3d(0));
        Mat4d matrix = Mat4d::Identity();
        for (int axis = 0; axis < 3; ++axis) {
            openvdb::Vec3d unit(0); unit[axis] = 1;
            const auto p = input.indexToWorld(unit) - origin;
            for (int row = 0; row < 3; ++row) matrix(row, axis) = p[row];
        }
        for (int row = 0; row < 3; ++row) matrix(row, 3) = origin[row] - 0.5 * matrix.row(row).head<3>().sum();
        auto corner_map = Transformd::from_matrix(matrix);
        if (!corner_map) return std::unexpected(corner_map.error());
        if (auto inverse = corner_map->inverse(); !inverse) return std::unexpected(inverse.error());

        // Validate the source before copying; inactive nonzero tiles must be included.
        std::uint64_t visits = 0;
        std::optional<Domain> occupied;
        for (auto it = input.tree().cbeginValueAll(); it; ++it) {
            if (++visits > options.limits.maximum_value_visits)
                return std::unexpected(Error{ErrorCode::invalid_argument, "OpenVDB normalization work limit exceeded"});
            const auto value = *it;
            if (value < 0 || static_cast<std::uint32_t>(value) > options.palette_size)
                return std::unexpected(Error{ErrorCode::invalid_argument, "OpenVDB value is outside the palette"});
            if (!value) continue;
            openvdb::CoordBBox box; it.getBoundingBox(box);
            Domain domain;
            for (int i = 0; i < 3; ++i) { domain.minimum[i] = box.min()[i]; domain.maximum[i] = box.max()[i]; }
            if (auto valid = validate(domain); !valid) return std::unexpected(valid.error());
            if (!occupied) occupied = domain;
            else for (int i = 0; i < 3; ++i) {
                occupied->minimum[i] = std::min(occupied->minimum[i], domain.minimum[i]);
                occupied->maximum[i] = std::max(occupied->maximum[i], domain.maximum[i]);
            }
        }
        const Domain domain = options.domain.value_or(occupied.value_or(Domain{}));
        if (occupied) for (int i = 0; i < 3; ++i)
            if (domain.minimum[i] > occupied->minimum[i] || domain.maximum[i] < occupied->maximum[i])
                return std::unexpected(Error{ErrorCode::invalid_argument, "Voxel domain omits nonzero source values"});

        auto normalized = input.deepCopy();
        // ValueAll is stable while toggling active flags; no nodes are inserted/removed here.
        for (auto it = normalized->tree().beginValueAll(); it; ++it) it.setActiveState(*it != 0);
        normalized->setName("dk-solid-voxels");
        const auto normalized_bytes = normalized->memUsage();
        if (normalized_bytes > options.limits.maximum_tree_bytes)
            return std::unexpected(Error{ErrorCode::invalid_argument, "Normalized OpenVDB tree exceeds budget"});
        auto storage = std::make_shared<geometry::voxels::detail::SparseStorage>();
        geometry::voxels::detail::LimitedNanoBuffer allocator(options.limits.maximum_nano_bytes);
        storage->handle = nanovdb::tools::createNanoGrid<openvdb::Int32Grid, std::int32_t, geometry::voxels::detail::LimitedNanoBuffer>(
            *normalized, nanovdb::tools::StatsMode::All, nanovdb::CheckMode::Full, 0, allocator);
        const auto* nano = storage->handle.grid<std::int32_t>();
        if (!nano) return std::unexpected(Error{ErrorCode::internal_error, "NanoVDB converter returned an incompatible grid"});
        storage->domain = domain;
        storage->corner_to_local = *corner_map;
        storage->statistics = {source_bytes, normalized_bytes, storage->handle.bufferSize(), visits, nano->activeVoxelCount(),
            {nano->tree().nodeCount(0), nano->tree().nodeCount(1), nano->tree().nodeCount(2)}};
        return geometry::voxels::detail::SparseFactory::publish(std::move(storage));
    } catch (const std::bad_alloc&) {
        return std::unexpected(Error{ErrorCode::internal_error, "OpenVDB conversion allocation failed"});
    } catch (const std::exception& exception) { return std::unexpected(conversion_error(exception)); }
}

Result<geometry::voxels::SparseGrid> build_voxel_grid(std::span<const VoxelBlock> blocks, const VoxelBuildOptions& options) {
    using namespace geometry::voxels;
    if (auto valid = validate_options(options); !valid) return std::unexpected(valid.error());
    if (blocks.size() > options.limits.maximum_blocks)
        return std::unexpected(Error{ErrorCode::invalid_argument, "Voxel source block limit exceeded"});
    for (const auto& block : blocks) {
        if (block.dimension != 1 && block.dimension != 8 && block.dimension != 128 && block.dimension != 4096)
            return std::unexpected(Error{ErrorCode::invalid_argument, "Voxel block dimension must be 1, 8, 128 or 4096"});
        if (block.value < 0 || static_cast<std::uint32_t>(block.value) > options.palette_size)
            return std::unexpected(Error{ErrorCode::invalid_argument, "Voxel block value is outside the palette"});
        for (int i = 0; i < 3; ++i)
            if (block.minimum[i] < -coordinate_limit || static_cast<std::int64_t>(block.minimum[i]) + block.dimension - 1 > coordinate_limit ||
                block.minimum[i] % static_cast<std::int32_t>(block.dimension) != 0)
                return std::unexpected(Error{ErrorCode::invalid_argument, "Voxel block is misaligned or outside coordinate range"});
    }
    try {
        static std::once_flag initialized;
        std::call_once(initialized, [] { openvdb::initialize(); });
        auto grid = openvdb::Int32Grid::create(0);
        openvdb::math::Mat4d map;
        for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) map(r, c) = options.sample_to_local.matrix()(c, r);
        grid->setTransform(openvdb::math::Transform::createLinearTransform(map));
        using Upper = openvdb::Int32Tree::RootNodeType::ChildNodeType;
        using Lower = Upper::ChildNodeType;
        using Leaf = Lower::ChildNodeType;
        // Conservative reserve before each edit: one path plus root/map bookkeeping.
        constexpr std::uint64_t path_reserve = sizeof(Upper) + sizeof(Lower) + sizeof(Leaf) + 4096;
        for (const auto& block : blocks) {
            const auto bytes = grid->memUsage();
            if (bytes > options.limits.maximum_tree_bytes || path_reserve > options.limits.maximum_tree_bytes - bytes)
                return std::unexpected(Error{ErrorCode::invalid_argument, "OpenVDB construction exceeds tree reserve budget"});
            const openvdb::Coord p(block.minimum[0], block.minimum[1], block.minimum[2]);
            if (block.dimension == 1) {
                grid->tree().setValue(p, block.value);
                if (!block.active) grid->tree().setValueOff(p);
            } else {
                const auto level = block.dimension == 8 ? 1u : (block.dimension == 128 ? 2u : 3u);
                grid->tree().addTile(level, p, block.value, block.active);
            }
        }
        return detail::convert_openvdb(*grid, options);
    } catch (const std::bad_alloc&) {
        return std::unexpected(Error{ErrorCode::internal_error, "OpenVDB construction allocation failed"});
    } catch (const std::exception& exception) { return std::unexpected(conversion_error(exception)); }
}
} // namespace dk::assets

#include <dk/geometry/Voxels.hpp>
#include "Traversal.hpp"
#include <limits>
#include <new>

namespace dk::geometry::voxels {
Result<void> validate(const Domain& domain) {
    for (int i = 0; i < 3; ++i)
        if (domain.minimum[i] > domain.maximum[i] || domain.minimum[i] < -coordinate_limit || domain.maximum[i] > coordinate_limit)
            return std::unexpected(Error{ErrorCode::invalid_argument, "Voxel domain must be ordered and within +/- 2^30"});
    return {};
}

Result<DenseGrid> DenseGrid::build(const DenseInput& input, DenseLimits limits) {
    if (!input.palette_size || input.palette_size > static_cast<std::uint32_t>(INT32_MAX))
        return std::unexpected(Error{ErrorCode::invalid_argument, "Voxel palette size must be in 1..INT32_MAX"});
    std::uint64_t count = 1;
    Domain domain{input.minimum, input.minimum};
    for (int i = 0; i < 3; ++i) {
        const auto size = input.dimensions[i];
        if (!size || count > limits.maximum_cells / size)
            return std::unexpected(Error{ErrorCode::invalid_argument, "Dense voxel dimensions exceed cell budget"});
        count *= size;
        const auto maximum = static_cast<std::int64_t>(input.minimum[i]) + size - 1;
        if (maximum > coordinate_limit)
            return std::unexpected(Error{ErrorCode::invalid_argument, "Dense voxel coordinate overflow"});
        domain.maximum[i] = static_cast<std::int32_t>(maximum);
    }
    if (auto valid = validate(domain); !valid) return std::unexpected(valid.error());
    if (count != input.values.size() || count > std::numeric_limits<std::size_t>::max() / sizeof(std::uint32_t))
        return std::unexpected(Error{ErrorCode::invalid_argument, "Dense voxel data length mismatch"});
    if (auto inverse = input.corner_to_local.inverse(); !inverse) return std::unexpected(inverse.error());
    for (const auto value : input.values)
        if (value > input.palette_size) return std::unexpected(Error{ErrorCode::invalid_argument, "Voxel value exceeds palette"});
    try {
        DenseGrid grid;
        grid.domain_ = domain;
        grid.dimensions_ = input.dimensions;
        grid.palette_size_ = input.palette_size;
        grid.corner_to_local_ = input.corner_to_local;
        grid.values_.assign(input.values.begin(), input.values.end());
        return grid;
    } catch (const std::bad_alloc&) {
        return std::unexpected(Error{ErrorCode::internal_error, "Dense voxel allocation failed"});
    }
}
std::uint32_t DenseGrid::value(Coord cell) const noexcept {
    if (!detail::contains(domain_, {cell[0], cell[1], cell[2]})) return 0;
    const auto x = static_cast<std::uint64_t>(static_cast<std::int64_t>(cell[0]) - domain_.minimum[0]);
    const auto y = static_cast<std::uint64_t>(static_cast<std::int64_t>(cell[1]) - domain_.minimum[1]);
    const auto z = static_cast<std::uint64_t>(static_cast<std::int64_t>(cell[2]) - domain_.minimum[2]);
    return values_[static_cast<std::size_t>(x + dimensions_[0] * (y + dimensions_[1] * z))];
}
Result<QueryResult> DenseGrid::nearest(const Ray& ray, const Transformd& world, QueryLimits limits) const {
    return detail::traverse(ray, domain_, corner_to_local_, world, limits, [&](Coord cell) { return detail::Region{value(cell), 1}; });
}
} // namespace dk::geometry::voxels

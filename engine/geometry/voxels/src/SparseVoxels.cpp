#include <dk/geometry/SparseVoxels.hpp>
#include "NanoStorage.hpp"
#include "Traversal.hpp"

namespace dk::geometry::voxels {
const Domain& SparseGrid::domain() const noexcept { return storage_->domain; }
const Transformd& SparseGrid::corner_to_local() const noexcept { return storage_->corner_to_local; }
const SparseStatistics& SparseGrid::statistics() const noexcept { return storage_->statistics; }
std::span<const std::byte> SparseGrid::bytes() const noexcept {
    return {static_cast<const std::byte*>(storage_->handle.data()), static_cast<std::size_t>(storage_->handle.bufferSize())};
}
std::uint32_t SparseGrid::value(Coord cell) const noexcept {
    if (!detail::contains(domain(), {cell[0], cell[1], cell[2]})) return 0;
    return static_cast<std::uint32_t>(storage_->handle.grid<std::int32_t>()->tree().getValue(nanovdb::Coord(cell[0], cell[1], cell[2])));
}
Result<QueryResult> SparseGrid::nearest(const Ray& ray, const Transformd& world, QueryLimits limits) const {
    const auto& tree = storage_->handle.grid<std::int32_t>()->tree();
    return detail::traverse(ray, domain(), corner_to_local(), world, limits, [&](Coord cell) {
        const nanovdb::Coord p(cell[0], cell[1], cell[2]);
        const auto dimension = tree.get<nanovdb::GetDim<std::int32_t>>(p);
        // An absent root entry is a uniform background cube of the root child size.
        return detail::Region{static_cast<std::uint32_t>(tree.getValue(p)), dimension ? dimension : 4096};
    });
}
} // namespace dk::geometry::voxels

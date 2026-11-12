#include <dk/geometry/SparseVoxels.hpp>
#include "NanoStorage.hpp"
#include "Traversal.hpp"

namespace dk::geometry::voxels {
Result<std::vector<Domain>> SparseGrid::surface_blocks(std::size_t maximum) const {
    if (!maximum || maximum>1024*1024) return std::unexpected(Error{ErrorCode::invalid_argument,"Invalid voxel block budget"});
    try {
        std::vector<Domain> blocks;
        const auto append=[&](nanovdb::Coord origin,std::uint32_t dim) {
            Domain block;
            for (unsigned axis=0;axis<3;++axis) {
                block.minimum[axis]=std::max(origin[axis],domain().minimum[axis]);
                block.maximum[axis]=std::int32_t(std::min(std::int64_t(origin[axis])+dim-1,std::int64_t(domain().maximum[axis])));
                if (block.minimum[axis]>block.maximum[axis]) return;
            }
            if (blocks.size()==maximum) throw Error{ErrorCode::invalid_argument,"Voxel block budget exceeded"};
            blocks.push_back(block);
        };
        const auto visit=[&](auto&& self,const auto& node)->void {
            using Node=std::remove_cvref_t<decltype(node)>;
            if constexpr(Node::LEVEL==0) {
                if (node.cbeginValueOn()) append(node.origin(),Node::DIM);
            } else {
                for(auto value=node.cbeginValueOn();value;++value) if (*value!=0) append(value.getCoord(),Node::ChildNodeType::DIM);
                for(auto child=node.cbeginChild();child;++child) self(self,*child);
            }
        };
        visit(visit,storage_->handle.grid<std::int32_t>()->tree().root());
        return blocks;
    } catch(const Error& error) { return std::unexpected(error); }
    catch(const std::bad_alloc&) { return std::unexpected(Error{ErrorCode::internal_error,"Voxel block allocation failed"}); }
}
const Domain& SparseGrid::domain() const noexcept { return storage_->domain; }
const Transformd& SparseGrid::corner_to_local() const noexcept { return storage_->corner_to_local; }
const SparseStatistics& SparseGrid::statistics() const noexcept { return storage_->statistics; }
std::uint32_t SparseGrid::maximum_value() const noexcept { return storage_->maximum_value; }
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

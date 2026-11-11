#include "NanoStorage.hpp"
#include <nanovdb/tools/GridChecksum.h>
#include <bit>
#include <cstring>
#include <cmath>

namespace dk::geometry::voxels {
namespace {
using Grid=nanovdb::NanoGrid<std::int32_t>;
using Root=Grid::TreeType::RootType;
using Upper=Root::ChildNodeType;
using Lower=Upper::ChildNodeType;
using Leaf=Lower::ChildNodeType;
void require(bool b,const char* message) { if(!b) throw Error{ErrorCode::invalid_argument,message}; }
struct Validator {
    const std::byte* bytes;
    Domain domain;
    std::uint32_t palette,maximum=0;
    std::uint64_t remaining,visits=0,active=0;
    std::array<std::size_t,3> offsets{},sizes{sizeof(Leaf),sizeof(Lower),sizeof(Upper)};
    std::array<std::uint32_t,3> counts{},next{},tiles{};
    void value(std::int32_t v,bool enabled,nanovdb::Coord origin,unsigned dimension) {
        require(remaining>0,"NanoVDB validation work budget exceeded"); --remaining; ++visits;
        require(v>=0 && static_cast<std::uint32_t>(v)<=palette && enabled==(v!=0),"Invalid or non-normalized NanoVDB value");
        if(!v) return;
        maximum=std::max(maximum,static_cast<std::uint32_t>(v));
        for(int a=0;a<3;++a) require(origin[a]>=domain.minimum[a] && std::int64_t(origin[a])+dimension-1<=domain.maximum[a],"NanoVDB occupied value outside domain");
        const auto n=std::uint64_t(dimension)*dimension*dimension;
        require(n<=UINT64_MAX-active,"NanoVDB active count overflow"); active+=n;
    }
    template<class Node> const Node* child(std::size_t parent,std::int64_t delta,nanovdb::Coord origin) {
        constexpr unsigned level=Node::LEVEL;
        require(next[level]<counts[level],"NanoVDB child count exceeds node array");
        const auto expected=offsets[level]+std::size_t(next[level]++)*sizes[level];
        require(delta>0 && static_cast<std::uint64_t>(delta)==expected-parent,"NanoVDB relative child address is not canonical");
        const auto* node=reinterpret_cast<const Node*>(bytes+expected);
        if constexpr(level==0) {
            require(node->origin()==origin,"NanoVDB leaf origin differs from parent address");
            const auto* data=node->data();
            for(unsigned i=0;i<512;++i) {
                const nanovdb::Coord p{origin[0]+int(i>>6),origin[1]+int((i>>3)&7),origin[2]+int(i&7)};
                value(data->mValues[i],data->mValueMask.isOn(i),p,1);
            }
        } else {
            using Child=typename Node::ChildNodeType;
            constexpr unsigned dimension=Child::DIM, log=Node::LOG2DIM, mask=(1u<<log)-1;
            const auto* data=node->data();
            const auto active_before=active;
            for(unsigned i=0;i<Node::SIZE;++i) {
                const nanovdb::Coord p{origin[0]+int((i>>(2*log))*dimension),origin[1]+int(((i>>log)&mask)*dimension),origin[2]+int((i&mask)*dimension)};
                if(data->mChildMask.isOn(i)) {
                    require(!data->mValueMask.isOn(i),"NanoVDB child also marked as active value");
                    child<Child>(expected,data->mTable[i].child,p);
                } else {
                    value(data->mTable[i].value,data->mValueMask.isOn(i),p,dimension);
                    if(data->mValueMask.isOn(i)) ++tiles[level-1];
                }
            }
            // GridStats replaces an all-zero internal node's bbox with the empty
            // sentinel, so its original coordinate cannot be recovered from bbox.min.
            // Parent masks/addresses are authoritative; only populated nodes have an origin.
            require(active==active_before ? data->mBBox.empty() : node->origin()==origin,
                "NanoVDB internal origin/empty bbox differs from validated subtree");
        }
        return node;
    }
};
}
std::uint32_t sparse_layout_version() noexcept { return nanovdb::Version{}.id(); }
Result<SparseGrid> load_sparse_grid(std::span<const std::byte> bytes,const Domain& domain,
    const Transformd& corner,std::uint32_t palette,SparseLoadLimits limits) {
    static_assert(std::endian::native==std::endian::little);
    if(auto valid=validate(domain); !valid) return std::unexpected(valid.error());
    if(auto inverse=corner.inverse(); !inverse) return std::unexpected(inverse.error());
    try {
        constexpr auto root_offset=sizeof(nanovdb::GridData)+sizeof(nanovdb::TreeData);
        require(palette>0 && palette<=INT32_MAX && limits.values && bytes.size()<=limits.bytes &&
            bytes.size()>=root_offset+sizeof(Root) && bytes.size()%32==0,"Invalid NanoVDB size or limits");
        detail::LimitedNanoBuffer allocator(limits.bytes); auto buffer=detail::LimitedNanoBuffer::create(bytes.size(),&allocator);
        std::memcpy(buffer.data(),bytes.data(),bytes.size()); const auto* base=static_cast<const std::byte*>(buffer.data());
        const auto* grid=reinterpret_cast<const Grid*>(base);
        require((grid->mMagic==NANOVDB_MAGIC_NUMB || grid->mMagic==NANOVDB_MAGIC_GRID) && grid->mGridSize==bytes.size() &&
            grid->mVersion.id()==sparse_layout_version() && grid->mGridCount==1 && grid->mGridIndex==0 && grid->mGridType==nanovdb::GridType::Int32 &&
            grid->mGridClass==nanovdb::GridClass::Unknown && grid->isBreadthFirst() && !grid->mBlindMetadataCount && !grid->hasLongGridName(),"Unsupported NanoVDB grid header/layout");
        require(std::memchr(grid->mGridName,0,sizeof(grid->mGridName))!=nullptr,"Unterminated NanoVDB name");
        const auto* tree=reinterpret_cast<const nanovdb::TreeData*>(base+sizeof(nanovdb::GridData));
        require(tree->mNodeOffset[3]==sizeof(nanovdb::TreeData),"Invalid NanoVDB root offset");
        const auto* root=reinterpret_cast<const Root::DataType*>(base+root_offset);
        require(root->mBackground==0 && root->mTableSize<=(bytes.size()-root_offset-sizeof(Root))/sizeof(Root::Tile),"Invalid NanoVDB root table");
        Validator v{base,domain,palette,0,limits.values};
        std::size_t cursor=root_offset+sizeof(Root)+std::size_t(root->mTableSize)*sizeof(Root::Tile);
        for(int level=2;level>=0;--level) {
            const auto count=tree->mNodeCount[level]; v.counts[level]=count; v.offsets[level]=cursor;
            require(count<=(bytes.size()-cursor)/v.sizes[level],"Truncated NanoVDB node array");
            require(tree->mNodeOffset[level]==static_cast<std::int64_t>(cursor-sizeof(nanovdb::GridData)) ||
                (!count && tree->mNodeOffset[level]==0),"Invalid NanoVDB node-array offset"); cursor+=std::size_t(count)*v.sizes[level];
        }
        require(cursor==bytes.size(),"Unexpected trailing NanoVDB data");
        for(unsigned i=0;i<root->mTableSize;++i) {
            const auto& tile=*root->tile(i); const auto origin=tile.origin();
            // OpenVDB emits signed lexicographic coordinates, not unsigned packed-key order.
            require(Root::DataType::CoordToKey(origin)==tile.key && (!i || root->tile(i-1)->origin()<origin),"Noncanonical NanoVDB root coordinates");
            // Bound parent coordinates before adding child offsets in signed arithmetic.
            for(int a=0;a<3;++a) require(origin[a]>=-coordinate_limit && origin[a]<=coordinate_limit,"NanoVDB root coordinate out of range");
            if(tile.child) v.child<Upper>(root_offset,tile.child,origin);
            else { require(tile.state<=1,"Invalid root tile active state"); v.value(tile.value,tile.state!=0,origin,Upper::DIM); if(tile.state) ++v.tiles[2]; }
        }
        require(v.next==v.counts && v.active==tree->mVoxelCount,"Unreachable NanoVDB nodes or active-count mismatch");
        for(int level=0;level<3;++level) require(v.tiles[level]==tree->mTileCount[level],"NanoVDB tile-count mismatch");
        // The serialized map must describe the same sample centers as the manifest's corner map.
        for(double x:grid->mMap.mMatD) require(std::isfinite(x),"Invalid NanoVDB map");
        for(double x:grid->mMap.mInvMatD) require(std::isfinite(x),"Invalid NanoVDB inverse map");
        for(double x:grid->mMap.mVecD) require(std::isfinite(x),"Invalid NanoVDB translation");
        for(int axis=-1;axis<3;++axis) {
            Vec3d p=Vec3d::Zero(); if(axis>=0) p[axis]=1;
            const auto actual=grid->indexToWorld(nanovdb::Vec3d{p.x(),p.y(),p.z()});
            const auto expected=corner.transform_point(p+Vec3d::Constant(.5)); require(bool(expected),"Invalid corner map");
            for(int a=0;a<3;++a) require(std::isfinite(actual[a]) && std::abs(actual[a]-(*expected)[a])<=1e-12*std::max(1.,std::abs((*expected)[a])),"NanoVDB map differs from artifact");
        }
        require(grid->mChecksum.isFull() && nanovdb::tools::validateChecksum(grid,nanovdb::CheckMode::Full),"NanoVDB full checksum missing or mismatched");
        auto storage=std::make_shared<detail::SparseStorage>(); storage->handle=nanovdb::GridHandle<detail::LimitedNanoBuffer>(std::move(buffer));
        storage->domain=domain; storage->corner_to_local=corner; storage->maximum_value=v.maximum;
        storage->statistics={0,0,bytes.size(),v.visits,v.active,v.counts}; return detail::SparseFactory::publish(std::move(storage));
    } catch(const Error& error) { return std::unexpected(error.with_context("load_sparse_grid")); }
      catch(const std::exception& e) { return std::unexpected(Error{ErrorCode::invalid_argument,e.what(),{"load_sparse_grid"}}); }
}
} // namespace dk::geometry::voxels

// Included inside dk::render after the shared upload/packing helpers.
namespace {
struct VoxelBuild {
    std::vector<Buffer> bounds;
    std::vector<AccelerationStructureInstance> instances;
};
VoxelBuild prepare_voxels(PathGpuState& state,SubmissionQueue& queue,PathGpuGeometry& target,const PathSceneState& scene,std::stop_token stop) {
    VoxelBuild result;
#if DK_PATH_VOXELS
    std::vector<std::byte> nano;
    std::vector<Float4> instances,palette;
    std::vector<std::array<std::int32_t,4>> blocks;
    const auto maximum=queue.device().adapter().properties.limits.maxStorageBufferRange;
    const auto& as_limits=queue.device().adapter().ray_tracing.acceleration_structure_limits;
    if(scene.voxels.size()>(1u<<21) || scene.voxels.size()>maximum/(13*16))
        throw path_invalid("Voxel instance metadata/primary tag capacity exceeded");
    std::vector<std::pair<const void*,std::uint32_t>> grids;
    const auto active=std::ranges::count_if(scene.voxels,[](const auto& v){return !v.blocks.empty();});
    if(std::uint64_t(active)+bool(target.blas)>as_limits.maxInstanceCount) throw path_invalid("Voxel TLAS instance limit exceeded");
    target.voxel_blas.resize(active); result.bounds.reserve(active); result.instances.reserve(active);
    for(std::size_t vi=0;vi<scene.voxels.size();++vi) {
        cancelled(stop); const auto& voxel=scene.voxels[vi]; const auto& grid=*voxel.grid;
        for(int axis=0;axis<3;++axis) if(grid.domain().minimum[axis]<-(1<<20) || grid.domain().maximum[axis]>=(1<<20))
            throw path_invalid("Voxel path domain exceeds signed 21-bit float traversal range");
        if(voxel.blocks.size()>as_limits.maxPrimitiveCount || voxel.blocks.size()>maximum/32)
            throw path_invalid("Voxel AABB/metadata device limit exceeded");
        const Mat4f forward=voxel.corner_world.matrix().cast<float>(),inverse=voxel.inverse.matrix().cast<float>();
        if(!forward.allFinite() || !inverse.allFinite()) throw path_invalid("Voxel transform is outside float range");
        for(unsigned corner=0;corner<8;++corner) {
            Vec4f p{0,0,0,1}; for(unsigned axis=0;axis<3;++axis) p[axis]=float((corner&(1u<<axis)) ? grid.domain().maximum[axis]+1 : grid.domain().minimum[axis]);
            const Vec4f world=forward*p,roundtrip=inverse*world;
            if(!world.allFinite() || (world.head<3>().array().abs()>1e6f).any() || (roundtrip-p).cwiseAbs().maxCoeff()>.0625f)
                throw path_invalid("Voxel transform loses cell precision in float");
            for(int axis=0;axis<3;++axis) {
                const float offset=32*std::numeric_limits<float>::epsilon()*(inverse.block<1,3>(axis,0).cwiseAbs().dot(world.head<3>().cwiseAbs())+std::abs(inverse(axis,3))+std::abs(p[axis])+1);
                if(offset>=.125f) throw path_invalid("Voxel transform cannot preserve subcell ray offsets in float");
                Vec4f next=p; next[axis]+=1; if((forward*next).head<3>()==world.head<3>()) throw path_invalid("Voxel cell collapses in float");
            }
        }
        std::uint32_t byte_offset=0;
        const auto prior=std::ranges::find(grids,grid.bytes().data(),&std::pair<const void*,std::uint32_t>::first);
        if(prior!=grids.end()) byte_offset=prior->second;
        else {
            if(grid.bytes().size()>maximum || nano.size()>maximum-grid.bytes().size() || grid.bytes().size()%32 || nano.size()%32)
                throw path_invalid("Packed NanoVDB buffer exceeds address/alignment/device limits");
            byte_offset=std::uint32_t(nano.size()); grids.emplace_back(grid.bytes().data(),byte_offset);
            nano.insert(nano.end(),grid.bytes().begin(),grid.bytes().end());
        }
        const auto palette_offset=std::uint32_t(palette.size()),block_offset=std::uint32_t(blocks.size());
        if(voxel.palette.size()>maximum/32 || palette.size()>maximum/16-2*voxel.palette.size()) throw path_invalid("Voxel palette exceeds device buffer limit");
        for(const auto& m:voxel.palette) {palette.push_back({m.base_color.x(),m.base_color.y(),m.base_color.z(),m.metallic});palette.push_back({m.roughness,0,0,0});}
        std::vector<vk::AabbPositionsKHR> bounds; bounds.reserve(voxel.blocks.size());
        for(const auto& b:voxel.blocks) {
            blocks.push_back({b[0],b[1],b[2],0}); blocks.push_back({b[4],b[5],b[6],0});
            bounds.emplace_back(float(b[0]),float(b[1]),float(b[2]),float(b[4])+1,float(b[5])+1,float(b[6])+1);
        }
        for(int row=0;row<3;++row) instances.push_back({forward(row,0),forward(row,1),forward(row,2),forward(row,3)});
        for(int row=0;row<3;++row) instances.push_back({inverse(row,0),inverse(row,1),inverse(row,2),inverse(row,3)});
        Mat3f normal=inverse.block<3,3>(0,0).transpose(); normal/=normal.cwiseAbs().maxCoeff();
        for(int row=0;row<3;++row) instances.push_back({normal(row,0),normal(row,1),normal(row,2),0});
        const auto& domain=grid.domain();
        instances.push_back(integers(std::uint32_t(domain.minimum[0]),std::uint32_t(domain.minimum[1]),std::uint32_t(domain.minimum[2]),0));
        instances.push_back(integers(std::uint32_t(domain.maximum[0]),std::uint32_t(domain.maximum[1]),std::uint32_t(domain.maximum[2]),0));
        instances.push_back(integers(byte_offset,std::uint32_t(grid.bytes().size()),palette_offset,block_offset));
        instances.push_back(integers(std::uint32_t(voxel.blocks.size()),scene.material_model==PathTraceMaterialModel::metallic_roughness ? 1u : 0u,0,0));
        if(bounds.empty()) continue;
        result.bounds.push_back(take(state.factory.create_aabb_input(bounds)));
        AabbGeometry description{&result.bounds.back(),0,sizeof(vk::AabbPositionsKHR),std::uint32_t(bounds.size()),true};
        const auto at=result.bounds.size()-1; target.voxel_blas[at]=take(state.factory.create_aabb_blas(std::span{&description,1}));
        AccelerationStructureInstance instance; instance.blas=&target.voxel_blas[at]; instance.custom_index=std::uint32_t(vi); instance.sbt_record_offset=2;
        for(int row=0;row<3;++row) for(int col=0;col<4;++col) instance.transform[row*4+col]=forward(row,col);
        result.instances.push_back(instance);
    }
    if(nano.empty()) nano.resize(32); if(instances.empty()) instances.resize(13); if(blocks.empty()) blocks.resize(2); if(palette.empty()) palette.resize(2);
    if(blocks.size()>maximum/16 || instances.size()>maximum/16) throw path_invalid("Voxel instance/block buffer exceeds device limit");
    target.voxel_data[0]=upload(state.factory,nano,vk::BufferUsageFlagBits::eStorageBuffer);
    target.voxel_data[1]=upload(state.factory,std::as_bytes(std::span{instances}),vk::BufferUsageFlagBits::eStorageBuffer);
    target.voxel_data[2]=upload(state.factory,std::as_bytes(std::span{blocks}),vk::BufferUsageFlagBits::eStorageBuffer);
    target.voxel_data[3]=upload(state.factory,std::as_bytes(std::span{palette}),vk::BufferUsageFlagBits::eStorageBuffer);
#else
    (void)state; (void)queue; (void)target; (void)scene; (void)stop;
#endif
    return result;
}
} // namespace

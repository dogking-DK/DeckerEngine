#pragma once
#include <dk/assets/VoxelBuilder.hpp>
#include <dk/render/PathTraceReference.hpp>
#include <dk/scene/SceneDocument.hpp>
#include <stdexcept>

namespace path_voxel_test {
using namespace dk;
using namespace dk::render;
struct Fixture {
    memory::ResourceHandle heap;
    std::unique_ptr<SceneDocument> document=SceneDocument::create().value();
    CpuAsset voxel,mesh;
    EntityId entity=document->create_entity().value(),mesh_entity=document->create_entity().value();
    Transformd world;
    explicit Fixture(memory::ResourceHandle h):heap(h) {
        voxel.voxel.emplace(); voxel.voxel->id=AssetId::generate().value(); mesh.mesh.id=AssetId::generate().value();
        voxel.voxel->palette={{Vec3f{.6f,.4f,.2f},0,1},{Vec3f{.2f,.5f,.8f},0,1}};
        document->set_asset_references(entity,{{voxel.voxel->id,AssetKind::voxel}}).value();
        document->set_asset_references(mesh_entity,{{mesh.mesh.id,AssetKind::mesh}}).value();
    }
    void grid(std::span<const assets::VoxelBlock> blocks,const Transformd& corner={}) {
        assets::VoxelBuildOptions options; options.palette_size=2;
        Trsd centered; centered.translation=Vec3d::Constant(.5);
        options.sample_to_local=corner.compose(Transformd::from_trs(centered).value()).value();
        auto result=assets::build_voxel_grid(blocks,options);
        if(!result) throw std::runtime_error(result.error().message);
        voxel.voxel->grid=std::make_shared<const geometry::voxels::SparseGrid>(std::move(*result));
    }
    void transform(const Trsd& value) {
        world=Transformd::from_trs(value).value(); document->set_local_transform(entity,value).value();
    }
    static void triangle(CpuAsset& asset,Vec3f a,Vec3f b,Vec3f c,const VoxelMaterialData& color,Vec3f emission=Vec3f::Zero()) {
        MaterialData m; m.id=AssetId::generate().value(); m.base_color={color.base_color.x(),color.base_color.y(),color.base_color.z(),1};
        m.metallic=color.metallic; m.roughness=color.roughness; m.double_sided=true; m.emissive=emission;
        asset.materials.push_back(m); MeshPrimitive p; p.positions={a,b,c}; p.indices={0,1,2}; p.material=m.id;
        asset.mesh.primitives.push_back(std::move(p));
    }
    void quad(float lo,float hi,float z,Vec3f color,Vec3f emission=Vec3f::Zero()) {
        VoxelMaterialData m{color,0,1}; triangle(mesh,{lo,lo,z},{hi,lo,z},{hi,hi,z},m,emission);
        triangle(mesh,{lo,lo,z},{hi,hi,z},{lo,hi,z},m,emission);
    }
    RenderScene source() const {return RenderScene::extract(heap,document->snapshot().value()).value();}
    PathTraceScene scene(PathTraceMaterialModel model=PathTraceMaterialModel::lambert,PathTraceSceneLimits limits={}) const {
        const std::array<const CpuAsset*,2> assets{&voxel,&mesh};
        auto result=PathTraceScene::create(heap,source(),assets,limits,model);
        if(!result) throw std::runtime_error(result.error().message); return *result;
    }
    // Independent correctness oracle: emit the six exposed faces of each cell as
    // triangles. No sparse query, DDA, HDDA, or production block enumeration.
    PathTraceScene oracle(PathTraceMaterialModel model=PathTraceMaterialModel::lambert) const {
        auto doc=SceneDocument::create().value(); CpuAsset faces=mesh;
        const auto& grid=*voxel.voxel->grid; const auto& domain=grid.domain();
        std::uint64_t count=1; for(int axis=0;axis<3;++axis) count*=std::uint64_t(domain.maximum[axis]-domain.minimum[axis]+1);
        if(count>8192) throw std::runtime_error("oracle fixture exceeds small-grid bound");
        for(int z=domain.minimum[2];z<=domain.maximum[2];++z)
        for(int y=domain.minimum[1];y<=domain.maximum[1];++y)
        for(int x=domain.minimum[0];x<=domain.maximum[0];++x) {
            const geometry::voxels::Coord cell{x,y,z}; const auto value=grid.value(cell); if(!value) continue;
            for(int axis=0;axis<3;++axis) for(int side=0;side<2;++side) {
                auto neighbor=cell; neighbor[axis]+=side ? 1 : -1; if(grid.value(neighbor)) continue;
                const int u=(axis+1)%3,v=(axis+2)%3; std::array<Vec3f,4> p;
                for(int k=0;k<4;++k) {
                    Vec3d point{double(x),double(y),double(z)}; point[axis]+=side;
                    point[u]+=(k==1 || k==2); point[v]+=(k==2 || k==3);
                    p[k]=world.transform_point(grid.corner_to_local().transform_point(point).value()).value().cast<float>();
                }
                if(!side) std::swap(p[1],p[3]);
                const auto& m=voxel.voxel->palette[value-1]; triangle(faces,p[0],p[1],p[2],m); triangle(faces,p[0],p[2],p[3],m);
            }
        }
        auto e=doc->create_entity().value(); doc->set_asset_references(e,{{faces.mesh.id,AssetKind::mesh}}).value();
        doc->set_environment(document->snapshot().value().environment()).value();
        const std::array<const CpuAsset*,1> assets{&faces};
        return PathTraceScene::create(heap,RenderScene::extract(heap,doc->snapshot().value()).value(),assets,{65536,65536},model).value();
    }
    RenderView view(std::uint32_t size=4) const {
        ViewDescription d{size,size}; Trsd camera; camera.translation={.5,.5,2}; d.camera_world=Transformd::from_trs(camera).value();
        d.projection.setZero(); d.projection(0,0)=2; d.projection(1,1)=-2;
        d.projection(2,2)=10.0/(.1-10); d.projection(2,3)=1.0/(.1-10); d.projection(3,2)=-1;
        return RenderView::create(source(),d).value();
    }
};
} // namespace path_voxel_test

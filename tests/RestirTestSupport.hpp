#pragma once
#include "RenderAssetSupport.hpp"
#include <dk/render/RestirReference.hpp>
#include <dk/scene/SceneDocument.hpp>
#include <array>
#include <cmath>
#include <cstdio>

namespace restir_test {
using namespace dk;
using namespace dk::render;
inline AssetId asset_id(unsigned index) {
    char id[37]; std::snprintf(id,sizeof(id),"19010000-0000-4000-8000-%012u",index);
    return AssetId::parse(id).value();
}
struct Fixture : RenderMemory {
    std::unique_ptr<SceneDocument> doc=SceneDocument::create(SceneId::parse("19010000-0000-4000-8000-000000000001").value()).value();
    CpuAsset asset;
    EntityId entity=EntityId::parse("19010000-0000-4000-8000-000000000002").value();
    Fixture() {
        asset.mesh.id=asset_id(3); doc->create_entity(entity).value();
        doc->set_asset_references(entity,{{asset.mesh.id,AssetKind::mesh}}).value();
    }
    void triangle(Vec3f a,Vec3f b,Vec3f c,Vec3f color={.6f,.6f,.6f},Vec3f emission={0,0,0},bool two_sided=false) {
        MaterialData m; m.id=asset_id(static_cast<unsigned>(asset.materials.size())+4); m.metallic=0;
        m.base_color={color.x(),color.y(),color.z(),1}; m.emissive=emission; m.double_sided=two_sided;
        MeshPrimitive p; p.material=m.id; p.positions={a,b,c}; p.indices={0,1,2};
        asset.materials.push_back(m); asset.mesh.primitives.push_back(std::move(p));
    }
    void quad(float lo,float hi,float z,Vec3f color,Vec3f emission={0,0,0},bool flip=false) {
        Vec3f a{lo,lo,z},b{hi,lo,z},c{hi,hi,z},d{lo,hi,z};
        if(flip) { triangle(a,c,b,color,emission); triangle(a,d,c,color,emission); }
        else { triangle(a,b,c,color,emission); triangle(a,c,d,color,emission); }
    }
    RenderScene source() { return RenderScene::extract(heap,doc->snapshot().value()).value(); }
    PathTraceScene scene(PathTraceMaterialModel model=PathTraceMaterialModel::lambert) {
        const std::array assets{static_cast<const CpuAsset*>(&asset)};
        return PathTraceScene::create(heap,source(),assets,{},model).value();
    }
    void many_lights(unsigned count) {
        quad(-8,8,0,{.6f,.6f,.6f});
        const unsigned columns=static_cast<unsigned>(std::ceil(std::sqrt(double(count))));
        const float step=4.f/columns;
        const float emission=8.f/(count*.5f*step*step); // Sum(area * radiance) = 8.
        for(unsigned i=0;i<count;++i) {
            const float x=-2+step*(i%columns),y=-2+step*(i/columns);
            triangle({x,y,2},{x,y+step,2},{x+step,y,2},{0,0,0},Vec3f::Constant(emission));
        }
    }
};
inline const PathTraceRay down{{0,0,1},{0,0,-1}};
inline PathTraceSettings settings(std::uint32_t depth=1) {
    PathTraceSettings s; s.max_bounces=depth; s.rr_start=0; return s;
}
} // namespace restir_test

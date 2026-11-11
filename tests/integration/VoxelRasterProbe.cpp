#include <dk/render/ScenePipeline.hpp>
#include <dk/render/VoxelGpu.hpp>
#include <dk/assets/VoxelBuilder.hpp>
#include <dk/scene/SceneDocument.hpp>
#include <dk/math/Projection.hpp>
#include <dk/memory/MemorySystem.hpp>
#include <dk/memory/Context.hpp>
#include "SubmissionInternal.hpp"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <numbers>
#include <stdexcept>

using namespace dk;
using namespace dk::graphics;
using namespace dk::render;
using namespace dk::assets;
namespace {
template<class T,class E> T take(std::expected<T,E> result) {
    if(!result) { if constexpr(std::is_same_v<E,Error>) throw std::runtime_error(result.error().message); else throw std::bad_alloc{}; }
    return std::move(*result);
}
void check(Result<void> r) { if(!r) throw std::runtime_error(r.error().message); }
void require(bool value,const char* message) { if(!value) throw std::runtime_error(message); }
struct Diagnostics { std::atomic<unsigned> errors=0,warnings=0,known=0; };
void diagnostic(void* pointer,const Diagnostic& message) noexcept {
    auto& d=*static_cast<Diagnostics*>(pointer);
    if(message.severity==VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) ++d.errors;
    if(message.severity==VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        ++d.warnings;
        if(message.name=="Loader Message" && message.message=="Layer VK_LAYER_AMD_switchable_graphics uses API version 1.3 which is older than the application specified API version of 1.4. May cause issues.") ++d.known;
    }
    if(message.severity>=VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) std::fprintf(stderr,"%.*s: %.*s\n",int(message.name.size()),message.name.data(),int(message.message.size()),message.message.data());
}
PFN_vkQueueSubmit2 native_submit=nullptr;
PFN_vkWaitSemaphores native_wait=nullptr;
PFN_vkGetSemaphoreCounterValue native_counter=nullptr;
bool fail_submit=false,force_timeout=false;
VKAPI_ATTR VkResult VKAPI_CALL submit(VkQueue q,std::uint32_t count,const VkSubmitInfo2* info,VkFence fence) {
    if(std::exchange(fail_submit,false)) return VK_ERROR_OUT_OF_HOST_MEMORY; return native_submit(q,count,info,fence);
}
VKAPI_ATTR VkResult VKAPI_CALL wait(VkDevice d,const VkSemaphoreWaitInfo* info,std::uint64_t timeout) { return force_timeout ? VK_TIMEOUT : native_wait(d,info,timeout); }
VKAPI_ATTR VkResult VKAPI_CALL counter(VkDevice d,VkSemaphore s,std::uint64_t* value) { if(force_timeout) { *value=0; return VK_SUCCESS; } return native_counter(d,s,value); }
std::uint32_t allocations(SubmissionQueue& q) { VmaTotalStatistics stats{}; vmaCalculateStatistics(q.device().allocator(),&stats); return stats.total.statistics.allocationCount; }
EntityId id(unsigned number) { char text[40]; std::snprintf(text,sizeof(text),"00000000-0000-4000-8000-%012u",number); return take(EntityId::parse(text)); }
Transformd transform(Vec3d translation,Vec3d scale=Vec3d::Ones()) { Trsd t; t.translation=translation; t.scale=scale; return take(Transformd::from_trs(t)); }
struct Mesh {
    CpuAsset asset;
    double x0,x1,y0,y1,z;
    Vec3d base,emissive;
    bool masked=false;
};
Mesh quad(double x0,double x1,double y0,double y1,double z,Vec3d base,Vec3d emission={0,0,0},bool masked=false) {
    Mesh m; m.x0=x0; m.x1=x1; m.y0=y0; m.y1=y1; m.z=z; m.base=base; m.emissive=emission; m.masked=masked;
    m.asset.mesh.id=take(AssetId::generate()); MeshPrimitive p;
    p.positions={{float(x0),float(y0),float(z)},{float(x1),float(y0),float(z)},{float(x1),float(y1),float(z)},{float(x0),float(y1),float(z)}};
    p.indices={0,1,2,0,2,3}; p.normals.assign(4,Vec3f{0,0,1}); p.texcoords={{0,0},{1,0},{1,1},{0,1}};
    MaterialData material; material.id=take(AssetId::generate()); material.base_color={float(base.x()),float(base.y()),float(base.z()),masked ? 0.0f : 1.0f};
    material.emissive=emission.cast<float>(); material.metallic=0; if(masked) material.alpha_mode=AlphaMode::mask;
    p.material=material.id; m.asset.materials.push_back(material); m.asset.mesh.primitives.push_back(std::move(p)); return m;
}
struct Fixture {
    std::unique_ptr<SceneDocument> scene=take(SceneDocument::create());
    GpuAssets meshes;
    std::vector<Mesh> source;
    std::unique_ptr<VoxelGpuCache> cache;
    std::shared_ptr<const VoxelGpuGrid> grid;
    std::array<VoxelMaterial,1> blue{{{{.125f,.25f,1},0,1}}},yellow{{{{1,.5f,.125f},0,1}}};
    std::vector<VoxelInstance> instances;
    Fixture(memory::ResourceHandle heap,SubmissionQueue& q) : meshes(take(GpuAssets::create(heap))),cache(take(VoxelGpuCache::create(q))) {
        source.push_back(quad(-1.25,-.25,-1.25,1.25,-3,{.5,.125,.125},{1,0,0}));
        source.push_back(quad(-1.75,1.75,-1.5,1.5,-7,{.125,.5,.125}));
        for(std::size_t i=0;i<source.size();++i) {
            check(scene->create_entity(id(unsigned(i)+1))); check(scene->set_asset_references(id(unsigned(i)+1),{{source[i].asset.mesh.id,AssetKind::mesh}}));
            auto uploaded=take(meshes.upload(q,source[i].asset)); require(take(uploaded.wait(q)),"Mesh upload incomplete");
        }
        std::vector<VoxelBlock> blocks;
        for(int z=0;z<64;z+=8) for(int y=0;y<64;y+=8) for(int x=0;x<64;x+=8) blocks.push_back({{x,y,z},8,1,false});
        VoxelBuildOptions options; options.sample_to_local=transform(Vec3d::Constant(.5));
        grid=take(cache->prepare(q,take(build_voxel_grid(blocks,options))));
        instances.push_back({id(10),grid,transform({-1,-1,-6},Vec3d::Constant(1./32)),blue});
        instances.push_back({id(20),grid,transform({.5,-.5,-3},Vec3d::Constant(1./64)),yellow});
    }
    RenderView view(memory::ResourceHandle heap,bool ortho,unsigned width=96,unsigned height=64,double near_plane=1,double far_plane=17,Transformd camera={}) {
        ViewDescription d{width,height,1,camera};
        d.projection=ortho ? take(orthographic_projection({5,near_plane,far_plane},double(width)/height)) : take(perspective_projection({std::numbers::pi/2,near_plane,far_plane},double(width)/height));
        return take(RenderView::create(take(RenderScene::extract(heap,take(scene->snapshot()))),d));
    }
};
struct Surface { double t; Vec3d normal,color; bool voxel; };
Vec3d shade(Vec3d color,const Vec3d& normal,const VoxelRenderSettings& s) {
    if(s.shading==VoxelShading::directional) color=color.cwiseProduct(s.irradiance.cast<double>())*(std::max(normal.dot(s.light_direction.cast<double>()),0.0)/std::numbers::pi);
    return color;
}
std::optional<Surface> voxel_oracle(const VoxelInstance& instance,const geometry::Ray& ray,const VoxelRenderSettings& s) {
    // Independent analytic box interval for a uniform 64^3 cube; no DDA/accessor calls.
    const auto inv=take(instance.world.inverse()); const auto o=take(inv.transform_point(ray.origin)),d=take(inv.transform_direction(ray.direction));
    std::array<double,3> entries{},exits{};
    double enter=-std::numeric_limits<double>::infinity(),leave=std::numeric_limits<double>::infinity();
    for(int axis=0;axis<3;++axis) {
        if(d[axis]==0) {
            // Double unprojection can place an exact parallel face ray a few ulps inside.
            const double slab_error=64*std::numeric_limits<double>::epsilon()*std::max(64.,std::abs(o[axis]));
            if(o[axis]<=slab_error || o[axis]>=64-slab_error) return {};
            entries[axis]=-std::numeric_limits<double>::infinity(); exits[axis]=std::numeric_limits<double>::infinity();
        } else {
            const double a=-o[axis]/d[axis],b=(64-o[axis])/d[axis]; entries[axis]=std::min(a,b); exits[axis]=std::max(a,b);
        }
        enter=std::max(enter,entries[axis]); leave=std::min(leave,exits[axis]);
    }
    // Unprojection roundoff must not turn an exact zero-length edge graze into a solid interval.
    const double error=64*std::numeric_limits<double>::epsilon()*std::max({1.,std::abs(enter),std::abs(leave)});
    if(leave-enter<=error) return {};
    const bool entering=enter>=ray.minimum; const double t=entering ? enter : leave;
    if(t<ray.minimum || t>ray.maximum) return {};
    int axis=0; const auto& planes=entering ? entries : exits;
    while(axis<2 && std::abs(planes[axis]-t)>error) ++axis;
    Vec3d n=Vec3d::Zero(); n[axis]=(d[axis]>0)==!entering ? 1 : -1;
    n=(inv.matrix().topLeftCorner<3,3>().transpose()*n).normalized();
    return Surface{t,n,shade(instance.palette[0].base_color.cast<double>(),n,s),true};
}
int tone(double value,double exposure) {
    const double x=value*exposure/(1+value*exposure),encoded=x<=.0031308 ? x*12.92 : 1.055*std::pow(x,1/2.4)-.055;
    return int(std::lround(encoded*255));
}
std::vector<std::byte> rgba(const RenderFrame& frame) {
    std::vector<std::byte> result(std::size_t(frame.info().width)*frame.info().height*4); check(frame.read_rgba8(result)); return result;
}
void compare(const RenderFrame& frame,const RenderView& view,std::span<const Mesh> meshes,std::span<const VoxelInstance> voxels,
    const VoxelRenderSettings& shading,const RenderSettings& settings,bool require_both=true) {
    const auto bytes=rgba(frame); const auto width=frame.info().width,height=frame.info().height; const auto count=std::size_t(width)*height;
    std::vector<std::array<float,4>> linear(settings.linear_readback ? count : 0); if(settings.linear_readback) check(frame.read_linear_rgba32f(linear));
    std::vector<float> depths(settings.depth_readback ? count : 0); if(settings.depth_readback) check(frame.read_depth32f(depths));
    const Mat4d unproject=view.world_to_clip().inverse(); std::size_t mesh_pixels=0,voxel_pixels=0;
    std::vector<const VoxelInstance*> sorted; for(const auto& v:voxels) sorted.push_back(&v); std::ranges::sort(sorted,{},[](const auto* v){return v->entity;});
    for(unsigned y=0;y<height;++y) for(unsigned x=0;x<width;++x) {
        const double sx=2*(x+.5)/width-1,sy=2*(y+.5)/height-1;
        Vec4d a=unproject*Vec4d{sx,sy,0,1},b=unproject*Vec4d{sx,sy,1,1}; a/=a.w(); b/=b.w();
        const Vec3d delta=(b-a).head<3>(); geometry::Ray ray{a.head<3>(),delta.normalized(),0,delta.norm()}; std::optional<Surface> best;
        for(const auto& m:meshes) {
            if(m.masked || ray.direction.z()==0) continue;
            const double t=(m.z-ray.origin.z())/ray.direction.z(); const Vec3d p=ray.origin+t*ray.direction;
            if(t<0 || t>ray.maximum || (best && t>=best->t) || p.x()<=m.x0 || p.x()>=m.x1 || p.y()<=m.y0 || p.y()>=m.y1) continue;
            best=Surface{t,{0,0,1},shade(m.base,{0,0,1},shading)+m.emissive,false};
        }
        for(const auto* v:sorted) if(auto hit=voxel_oracle(*v,ray,shading); hit && (!best || hit->t<best->t-1e-10)) best=*hit;
        const Vec3d color=best ? best->color : settings.clear_rgb.cast<double>(); double depth=1;
        if(best) {
            const Vec3d p=ray.origin+ray.direction*best->t; const Vec4d clip=view.world_to_clip()*Vec4d{p.x(),p.y(),p.z(),1}; depth=clip.z()/clip.w();
            if(best->voxel) ++voxel_pixels; else ++mesh_pixels;
        }
        const std::size_t pixel=std::size_t(y)*width+x;
        for(unsigned c=0;c<3;++c) {
            if(std::abs(std::to_integer<int>(bytes[4*pixel+c])-tone(color[c],settings.exposure))>1)
                throw std::runtime_error("LDR mismatch at "+std::to_string(x)+","+std::to_string(y)+" channel "+std::to_string(c)+" actual="+std::to_string(std::to_integer<int>(bytes[4*pixel+c]))+" expected="+std::to_string(tone(color[c],settings.exposure)));
            if(settings.linear_readback && std::abs(linear[pixel][c]-color[c])>2e-4*std::max(1.,std::abs(color[c]))) throw std::runtime_error("HDR mismatch at "+std::to_string(pixel));
        }
        require(bytes[4*pixel+3]==std::byte{255},"Alpha mismatch");
        if(settings.linear_readback) require(linear[pixel][3]==1,"HDR alpha mismatch");
        if(settings.depth_readback && std::abs(depths[pixel]-depth)>2e-6) throw std::runtime_error("Final D32 mismatch at "+std::to_string(pixel));
    }
    if(require_both) require(mesh_pixels>0 && voxel_pixels>0,"Fixture did not exercise both mesh and voxel visibility");
    std::printf("pixels=%zu mesh=%zu voxel=%zu HDR=%d D32=%d exposure=%.2f\n",count,mesh_pixels,voxel_pixels,settings.linear_readback,settings.depth_readback,settings.exposure);
}
void save(const RenderFrame& frame,const char* name) {
    std::filesystem::create_directories(DK_VOXEL_ARTIFACT_DIR); const auto data=rgba(frame);
    std::ofstream f(std::filesystem::path{DK_VOXEL_ARTIFACT_DIR}/name,std::ios::binary); f<<"P6\n"<<frame.info().width<<' '<<frame.info().height<<"\n255\n";
    for(std::size_t i=0;i<data.size();i+=4) f.write(reinterpret_cast<const char*>(data.data()+i),3); require(bool(f),"Image write failed");
    std::ofstream plan(std::filesystem::path{DK_VOXEL_ARTIFACT_DIR}/"voxel-composition-plan.txt"); plan<<frame.plan_text(); require(bool(plan),"Plan write failed");
}
void composition(memory::ResourceHandle heap,SubmissionQueue& q) {
    Fixture f(heap,q); auto pipeline=take(ScenePipeline::create(heap,q,DK_RENDER_SHADER_DIR));
    RenderSettings settings{{.03125f,.0625f,.125f},1,true,true,true,true}; VoxelRenderSettings shading;
    for(bool ortho:{false,true}) {
        auto view=f.view(heap,ortho);
        for(auto mode:{VoxelShading::unlit,VoxelShading::directional}) for(float exposure:{.5f,2.f}) {
            shading.shading=mode; shading.light_direction={.6f,0,.8f}; shading.irradiance={8,4,2}; settings.exposure=exposure;
            auto frame=take(pipeline.render_voxels(q,view,f.meshes,f.instances,shading,settings)); require(take(frame.wait(q)),"Mixed frame incomplete");
            compare(frame,view,f.source,f.instances,shading,settings);
            require(frame.info().voxel_count==2 && frame.info().draw_count==2,"Mixed metadata mismatch");
            for(const char* name:{"opaque","voxel HDDA 0","voxel depth HDR composite","final depth diagnostic","tone mapping"}) require(frame.plan_text().find(name)!=std::string_view::npos,"Missing composition pass");
        }
        settings.linear_readback=settings.depth_readback=false; settings.exposure=1; shading.shading=VoxelShading::unlit;
        auto large=f.view(heap,ortho,640,480); auto frame=take(pipeline.render_voxels(q,large,f.meshes,f.instances,shading,settings)); require(take(frame.wait(q)),"Large frame incomplete");
        compare(frame,large,f.source,f.instances,shading,settings); save(frame,ortho ? "voxel-mixed-orthographic.ppm" : "voxel-mixed-perspective.ppm");
        settings.linear_readback=settings.depth_readback=true;
    }
    // Same-depth mesh wins. Identical voxel instances choose stable ID, independent of input order.
    f.source[0]=quad(-1.25,1.25,-1.25,1.25,-4,{.5,.125,.125}); f.source[1].masked=true;
    f.source[1].asset.materials[0].alpha_mode=AlphaMode::mask; f.source[1].asset.materials[0].base_color.w()=0;
    for(std::size_t i=0;i<2;++i) { check(f.scene->set_asset_references(id(unsigned(i)+1),{{f.source[i].asset.mesh.id,AssetKind::mesh}})); auto upload=take(f.meshes.upload(q,f.source[i].asset)); require(take(upload.wait(q)),"Tie mesh upload incomplete"); }
    f.instances[1].world=f.instances[0].world;
    for(bool ortho:{false,true}) {
        auto view=f.view(heap,ortho,97,65); auto frame=take(pipeline.render_voxels(q,view,f.meshes,f.instances,shading,settings)); require(take(frame.wait(q)),"Tie frame incomplete");
        compare(frame,view,f.source,f.instances,shading,settings,false);
        auto empty=take(SceneDocument::create()); auto only=take(RenderView::create(take(RenderScene::extract(heap,take(empty->snapshot()))),view.description()));
        auto ordered=take(pipeline.render_voxels(q,only,f.meshes,f.instances,shading,settings)); require(take(ordered.wait(q)),"Ordered frame incomplete");
        compare(ordered,only,{},f.instances,shading,settings,false); const auto expected=rgba(ordered);
        std::ranges::reverse(f.instances); auto reversed=take(pipeline.render_voxels(q,only,f.meshes,f.instances,shading,settings)); require(take(reversed.wait(q)),"Reverse frame incomplete");
        require(rgba(reversed)==expected,"Instance tie depends on caller order"); std::ranges::reverse(f.instances);
    }
    // Clipping into solid returns the exit, including cameras inside; affine instance and offset/flip-Y projections.
    auto empty=take(SceneDocument::create()); auto scene=take(RenderScene::extract(heap,take(empty->snapshot())));
    f.instances.resize(1); shading.shading=VoxelShading::directional;
    for(bool ortho:{false,true}) for(unsigned variant=0;variant<4;++variant) {
        auto camera=variant==0 ? transform({.125,.25,-4.5}) : Transformd{};
        auto initial=f.view(heap,ortho,80,60,variant==1 ? 5 : .25,variant==2 ? 3 : 17,camera); auto d=initial.description();
        f.instances[0].world=transform({-1,-1,-6},Vec3d::Constant(1./32));
        if(variant==3) { Mat4d m=f.instances[0].world.matrix(); m(0,0)=-1./32; m(0,2)=1./64; m(0,3)=.5; f.instances[0].world=take(Transformd::from_matrix(m)); d.projection(1,1)*=-1; if(!ortho) d.projection(0,2)=.125; }
        auto view=take(RenderView::create(scene,d)); auto frame=take(pipeline.render_voxels(q,view,f.meshes,f.instances,shading,settings)); require(take(frame.wait(q)),"Clip/affine frame incomplete");
        compare(frame,view,{},f.instances,shading,settings,false);
    }
    shading.shading=VoxelShading::unlit; auto view=f.view(heap,false);
    auto legacy=take(pipeline.render(q,view,f.meshes,settings)); auto empty_voxels=take(pipeline.render_voxels(q,view,f.meshes,{},shading,settings));
    require(take(legacy.wait(q)) && take(empty_voxels.wait(q)),"Empty path incomplete"); require(rgba(legacy)==rgba(empty_voxels),"Empty voxel path changed mesh pixels");
}
void lifetime(memory::MemorySystem& memory,memory::ResourceHandle heap,SubmissionQueue& q) {
    Fixture f(heap,q); auto pipeline=take(ScenePipeline::create(heap,q,DK_RENDER_SHADER_DIR)); auto view=f.view(heap,false);
    VoxelRenderSettings shading; RenderSettings settings{{.03125f,.0625f,.125f},1,true,true,true,true};
    auto first=take(pipeline.render_voxels(q,view,f.meshes,f.instances,shading,settings)); require(take(first.wait(q)),"Initial frame incomplete"); const auto old=rgba(first);
    const auto base=allocations(q); const auto submitted=q.stats().submitted;
    auto unchanged=[&] { require(allocations(q)==base && q.stats().submitted==submitted && rgba(first)==old,"Rejected frame changed old state or leaked/submitted"); };
    auto duplicate=f.instances; duplicate[1].entity=duplicate[0].entity; require(!pipeline.render_voxels(q,view,f.meshes,duplicate,shading,settings),"Duplicate ID accepted"); unchanged();
    auto invalid=f.instances; invalid[0].palette={}; require(!pipeline.render_voxels(q,view,f.meshes,invalid,shading,settings),"Missing palette accepted"); unchanged();
    f.blue[0].base_color.x()=2; require(!pipeline.render_voxels(q,view,f.meshes,f.instances,shading,settings),"Bad palette accepted"); f.blue[0].base_color.x()=.125f; unchanged();
    auto lit=shading; lit.shading=VoxelShading::directional; f.blue[0].metallic=1;
    require(!pipeline.render_voxels(q,view,f.meshes,f.instances,lit,settings),"Metallic Lambert palette accepted"); f.blue[0].metallic=0; unchanged();
    auto tiny=shading; tiny.payload_bytes=1; require(!pipeline.render_voxels(q,view,f.meshes,f.instances,tiny,settings),"Payload budget ignored"); unchanged();
    auto invalid_view=view.description(); invalid_view.projection=Mat4d::Identity(); auto bad=take(RenderView::create(view.scene(),invalid_view));
    require(!pipeline.render_voxels(q,bad,f.meshes,f.instances,shading,settings),"Unsupported projection accepted"); unchanged();
    invalid=f.instances; invalid[0].world=transform({0,0,0},{0,1,1}); require(!pipeline.render_voxels(q,view,f.meshes,invalid,shading,settings),"Singular instance accepted"); unchanged();
    auto environment=f.scene->environment(); environment.enabled=true; check(f.scene->set_environment(environment));
    require(!pipeline.render_voxels(q,f.view(heap,false),f.meshes,f.instances,shading,settings),"Atmosphere combination silently accepted"); environment.enabled=false; check(f.scene->set_environment(environment)); unchanged();
    fail_submit=true; require(!pipeline.render_voxels(q,view,f.meshes,f.instances,shading,settings) && !fail_submit,"Submit failure ignored"); unchanged();
    {
        const std::array blocks{VoxelBlock{{0,0,0},1,1,false},VoxelBlock{{999,0,0},1,2,false}}; VoxelBuildOptions options; options.palette_size=2;
        auto cache=take(VoxelGpuCache::create(q)); auto grid=take(cache->prepare(q,take(build_voxel_grid(blocks,options)))); require(take(q.wait(grid->upload_submission())),"Palette fixture upload incomplete");
        const std::array instances{VoxelInstance{id(99),grid,{},f.blue}}; const auto before=q.stats().submitted;
        require(!pipeline.render_voxels(q,view,f.meshes,instances,shading,settings) && q.stats().submitted==before,"Invisible out-of-palette value was accepted");
    }
    auto pending=take(pipeline.render_voxels(q,view,f.meshes,f.instances,shading,settings)); force_timeout=true; auto timeout=pending.wait(q,0); force_timeout=false;
    require(timeout && !*timeout,"Timeout claimed completion"); f.blue[0].base_color={1,1,1}; require(take(pending.wait(q)) && rgba(pending)==old,"Submitted palette was not immutable"); f.blue[0].base_color={.125f,.25f,1}; pending={};
    {
        const std::array blocks{VoxelBlock{{0,0,-64},1,1,false},VoxelBlock{{0,0,0},1,1,false}};
        auto cache=take(VoxelGpuCache::create(q)); auto grid=take(cache->prepare(q,take(build_voxel_grid(blocks))));
        std::array instances{VoxelInstance{id(99),grid,{},f.blue}}; auto low=shading; low.maximum_regions=1;
        auto empty=take(SceneDocument::create()); auto d=f.view(heap,false,1,1).description(); auto v=take(RenderView::create(take(RenderScene::extract(heap,take(empty->snapshot()))),d));
        auto failed=take(pipeline.render_voxels(q,v,f.meshes,instances,low,settings)); require(!failed.wait(q),"GPU work-limit failure was silently rendered");
        std::array<std::byte,4> output; output.fill(std::byte{0x5a}); require(!failed.read_rgba8(output) && output[0]==std::byte{0x5a} && !failed.color(),"Failed GPU frame published pixels");
    }
    {
        // An invertible double transform may collapse all ray directions when packed as float.
        const std::array instances{VoxelInstance{id(99),f.grid,transform(Vec3d::Constant(-1e50),Vec3d::Constant(1e50)),f.blue}};
        auto empty=take(SceneDocument::create()); auto d=f.view(heap,false,1,1).description();
        auto v=take(RenderView::create(take(RenderScene::extract(heap,take(empty->snapshot()))),d));
        auto failed=take(pipeline.render_voxels(q,v,f.meshes,instances,shading,settings));
        require(!failed.wait(q) && !failed.color(),"Float ray underflow was silently rendered");
    }
    auto closing=take(pipeline.render_voxels(q,view,f.meshes,f.instances,shading,settings));
    std::weak_ptr<const VoxelGpuGrid> retained=f.grid; f.instances.clear(); duplicate.clear(); invalid.clear(); f.grid.reset(); f.cache.reset(); f.meshes.clear();
    require(!retained.expired(),"Pending frame dropped grid owner"); require(!memory.try_close().closed(),"Memory closed with live frame");
    require(!pipeline.render_voxels(q,view,f.meshes,{},shading,settings),"Closing accepted new work"); pipeline={}; first={};
    require(take(closing.wait(q)) && rgba(closing)==old,"Owner release/closing damaged submitted frame"); closing={}; check(q.poll()); require(retained.expired(),"Retired grid retained after frame release");
    std::printf("rejection/submit/timeout/immutable palette/GPU status/owners/closing passed\n");
}
}
int main(int argc,char** argv) {
    Diagnostics diagnostics; auto memory=take(memory::MemorySystem::create()); auto heap=take(memory.create_heap({"voxel-raster",memory::DomainCategory::render}));
    try {
        {
            memory::ThreadContext context{memory}; memory::ExecutionScope scope{context,heap};
            DeviceOptions options; options.validation=ValidationMode::required; options.diagnostic_sink=diagnostic; options.diagnostic_user_data=&diagnostics;
            auto device=take(Device::create(heap,options)); require(!device.ray_tracing_enabled(),"Raster unexpectedly enabled RT");
            std::printf("GPU=%s driver=%s RT=disabled validation=required\n",device.adapter().properties.deviceName.data(),device.adapter().driver.driverInfo.data());
            native_submit=reinterpret_cast<PFN_vkQueueSubmit2>(device.device_proc("vkQueueSubmit2")); native_wait=reinterpret_cast<PFN_vkWaitSemaphores>(device.device_proc("vkWaitSemaphores")); native_counter=reinterpret_cast<PFN_vkGetSemaphoreCounterValue>(device.device_proc("vkGetSemaphoreCounterValue"));
            auto q=take(graphics::detail::SubmissionAccess::create(heap,std::move(device),3,{submit,counter,wait}));
            if(argc>1 && std::string_view{argv[1]}=="lifetime") lifetime(memory,heap,q); else composition(heap,q);
            check(q.poll()); require(!q.stats().pending_slots && !q.stats().recording_slots && allocations(q)==0,"Composition GPU resources leaked"); check(q.close());
        }
        std::printf("validation errors=%u warnings=%u known=%u live=%zu\n",diagnostics.errors.load(),diagnostics.warnings.load(),diagnostics.known.load(),heap.snapshot().live_allocations);
        return !diagnostics.errors && diagnostics.warnings==diagnostics.known && !heap.snapshot().live_allocations && memory.try_close().closed() ? 0 : 1;
    } catch(const std::exception& error) { std::fprintf(stderr,"voxel raster failed: %s\n",error.what()); return 1; }
}

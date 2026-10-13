#include "../RenderAssetSupport.hpp"
#include <dk/render/ScenePipeline.hpp>
#include <dk/render/Atmosphere.hpp>
#include <dk/scene/SceneDocument.hpp>
#include "SubmissionInternal.hpp"
#include "ObjectInternal.hpp"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <fstream>
#include <numbers>
#include <stdexcept>
#include <vector>
using namespace dk;
using namespace dk::render;
using namespace dk::graphics;
namespace {
template<class T,class E> T take(std::expected<T,E> r) {
    if(!r) { if constexpr(std::is_same_v<E,Error>) throw std::runtime_error(r.error().message); else throw std::bad_alloc{}; }
    return std::move(*r);
}
void check(Result<void> r) { if(!r) throw std::runtime_error(r.error().message); }
void require(bool b,const char* m) { if(!b) throw std::runtime_error(m); }
struct Diagnostics { std::atomic<unsigned> errors=0,warnings=0; };
void diagnostic(void* user,const Diagnostic& d) noexcept {
    auto& count=*static_cast<Diagnostics*>(user);
    if(d.severity==VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) ++count.errors;
    if(d.severity==VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) ++count.warnings;
    if(d.severity>=VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
        std::fprintf(stderr,"%.*s\n",int(d.message.size()),d.message.data());
}
PFN_vkQueueSubmit2 native_submit;
bool fail_submit=false;
VKAPI_ATTR VkResult VKAPI_CALL submit(VkQueue q,std::uint32_t n,const VkSubmitInfo2* p,VkFence f) {
    return std::exchange(fail_submit,false) ? VK_ERROR_OUT_OF_HOST_MEMORY : native_submit(q,n,p,f);
}
unsigned allocations(const SubmissionQueue& q) {
    VmaTotalStatistics s{}; vmaCalculateStatistics(q.device().allocator(),&s); return s.total.statistics.allocationCount;
}
EnvironmentDescription vacuum() {
    EnvironmentDescription e; e.enabled=true;
    e.atmosphere.rayleigh_scattering_per_m={}; e.atmosphere.mie_scattering_per_m={};
    e.atmosphere.mie_extinction_per_m={}; e.atmosphere.absorption_extinction_per_m={};
    e.sun.direction={.3,.4,1}; e.sun.irradiance={1,2,3}; return take(normalize_environment(e));
}
struct Fixture {
    memory::ResourceHandle heap;
    CpuAsset asset;
    std::unique_ptr<SceneDocument> scene=take(SceneDocument::create());
    EntityId entity=take(EntityId::generate());
    GpuAssets assets; ScenePipeline pipeline; AtmosphereRenderer atmosphere;
    Vec3d anchor{0,10000,0}; double distance=5000,near_m=10,far_m=50000;
    double offset_x=0,offset_y=0;
    Quatd rotation=Quatd::Identity();
    RenderSettings settings{{.01f,.02f,.03f},1,true,false,true};
    AtmosphereSettings air{false,false,false};
    Fixture(memory::ResourceHandle h,SubmissionQueue& q):heap(h),assets(take(GpuAssets::create(h))),
        pipeline(take(ScenePipeline::create(h,q,DK_RENDER_SHADER_DIR))),
        atmosphere(take(AtmosphereRenderer::create(h,q,DK_ATMOSPHERE_SHADER_DIR))) {
        asset.mesh.id=take(AssetId::generate());
        MeshPrimitive p; p.positions={{-.8f,-.8f,0},{.8f,-.8f,0},{.8f,.8f,0},{-.8f,.8f,0}};
        p.normals.assign(4,Vec3f{0,0,1}); p.texcoords={{0,0},{1,0},{1,1},{0,1}}; p.indices={0,1,2,0,2,3};
        MaterialData m; m.id=take(AssetId::generate()); m.base_color={.8f,.5f,.25f,1}; m.emissive={.01f,.02f,.03f}; m.double_sided=true;
        p.material=m.id; asset.materials.push_back(m); asset.mesh.primitives.push_back(p);
        check(scene->create_entity(entity)); check(scene->set_asset_references(entity,{{asset.mesh.id,AssetKind::mesh}})); place(); upload(q);
    }
    void place() { Trsd t; t.translation=anchor+rotation*Vec3d{0,0,-distance}; t.rotation=rotation; t.scale=Vec3d::Constant(distance); check(scene->set_local_transform(entity,t)); }
    void upload(SubmissionQueue& q) { auto a=take(assets.upload(q,asset)); require(take(a.wait(q)),"upload wait"); }
    RenderView view() {
        ViewDescription d{33,33}; Trsd camera; camera.translation=anchor; camera.rotation=rotation;
        d.camera_world=take(Transformd::from_trs(camera)); d.projection=Mat4d::Zero();
        d.projection(0,0)=1; d.projection(1,1)=-1; d.projection(0,2)=offset_x; d.projection(1,2)=offset_y;
        d.projection(2,2)=far_m/(near_m-far_m); d.projection(2,3)=near_m*far_m/(near_m-far_m); d.projection(3,2)=-1;
        return take(RenderView::create(take(RenderScene::extract(heap,take(scene->snapshot()))),d));
    }
    AtmosphereSky prepare(SubmissionQueue& q,EnvironmentDescription e,bool multiple=false) {
        check(scene->set_environment(e)); return take(atmosphere.prepare(q,scene->environment(),anchor,multiple));
    }
    RenderFrame render(SubmissionQueue& q,const AtmosphereSky& sky) {
        auto f=take(pipeline.render_atmosphere(q,view(),assets,sky,settings,air)); require(take(f.wait(q)),"frame wait"); return f;
    }
};
using Pixel=std::array<float,4>;
std::vector<Pixel> linear(const RenderFrame& f) {
    std::vector<Pixel> p(std::size_t(f.info().width)*f.info().height); check(f.read_linear_rgba32f(p)); return p;
}
std::vector<std::byte> pixels(const RenderFrame& f) {
    std::vector<std::byte> p(std::size_t(f.info().width)*f.info().height*4); check(f.read_rgba8(p)); return p;
}
unsigned comparisons=0,max_byte_error=0;
int encoded(double x,double exposure) {
    x=x*exposure/(1+x*exposure); return int(std::lround(255*(x<=.0031308 ? 12.92*x : 1.055*std::pow(x,1/2.4)-.055)));
}
void expect(const RenderFrame& f,const Vec3d& radiance,double exposure=1,unsigned x=16,unsigned y=16) {
    const auto p=pixels(f); const auto offset=(std::size_t(y)*f.info().width+x)*4;
    for(unsigned c=0;c<3;++c) {
        const int actual=std::to_integer<int>(p[offset+c]),reference=encoded(radiance[c],exposure);
        ++comparisons; max_byte_error=std::max(max_byte_error,unsigned(std::abs(actual-reference)));
        if(std::abs(actual-reference)>2) throw std::runtime_error("radiance pixel channel "+std::to_string(c)+" actual="+std::to_string(actual)+" expected="+std::to_string(reference));
    }
    require(p[offset+3]==std::byte{255},"alpha not one");
}
void save(const RenderFrame& f,const std::string& name) {
    const auto p=pixels(f); std::ofstream out{name,std::ios::binary}; out<<"P6\n"<<f.info().width<<' '<<f.info().height<<"\n255\n";
    for(std::size_t i=0;i<p.size();i+=4) out.write(reinterpret_cast<const char*>(p.data()+i),3);
    require(bool(out),"sample write failed");
}

Vec3d ray(const Fixture& f,unsigned x,unsigned y) {
    return f.rotation*Vec3d{(x+.5)/33*2-1+f.offset_x,-((y+.5)/33*2-1+f.offset_y),-1};
}
void compare(const std::vector<Pixel>& actual,const std::vector<Pixel>& expected,const char* name) {
    std::vector<double> luminance,errors; double squares=0,maximum=0,edge=0;
    for(const auto& pixel:expected) luminance.push_back(.2126*pixel[0]+.7152*pixel[1]+.0722*pixel[2]);
    std::ranges::sort(luminance); const double scale=std::max(.001,luminance[std::size_t(.95*(luminance.size()-1))]);
    for(std::size_t i=0;i<actual.size();++i) {
        require(actual[i][3]==1,"linear alpha not one");
        for(unsigned c=0;c<3;++c) {
            require(std::isfinite(actual[i][c]) && actual[i][c]>=0,"invalid radiance");
            const double error=std::abs(actual[i][c]-expected[i][c])/scale;
            errors.push_back(error); squares+=error*error; maximum=std::max(maximum,error);
            const auto x=i%33,y=i/33;
            if(x==3 || x==4 || x==28 || x==29 || y==3 || y==4 || y==28 || y==29 || y==16) edge=std::max(edge,error);
        }
    }
    std::ranges::sort(errors); const double rmse=std::sqrt(squares/errors.size()),p99=errors[std::size_t(.99*(errors.size()-1))];
    std::printf("%s scale=%.6g nRMSE=%.6g p99=%.6g max=%.6g edge/horizon=%.6g\n",name,scale,rmse,p99,maximum,edge);
    require(rmse<=.03 && p99<=.08,"linear radiance reference threshold");
    require(edge<=.08,"edge/horizon reference threshold");
}
void cpu_spots(Fixture& f,const RenderFrame& frame) {
    const auto p=linear(frame); const auto env=f.scene->environment();
    const Vec3d surface=(f.asset.materials[0].base_color.head<3>()+f.asset.materials[0].emissive).cast<double>();
    for(auto xy : {std::array<unsigned,2>{16,16},{6,16},{26,16},{16,6},{16,26}}) {
        const auto direction=ray(f,xy[0],xy[1]); const double length=f.distance*direction.norm();
        const auto reference=take(integrate_atmosphere_reference(env,f.anchor,direction.normalized(),{1024,1024,length,false}));
        const Vec3d T=reference.ground && reference.distance_m<length*.999999 ? Vec3d::Zero().eval() : reference.transmittance;
        const Vec3d expected=reference.radiance+T.cwiseProduct(surface);
        const auto& pixel=p[xy[1]*33+xy[0]]; const Vec3d actual{pixel[0],pixel[1],pixel[2]};
        const double error=(actual-expected).cwiseAbs().maxCoeff()/std::max(.001,expected.maxCoeff());
        std::printf("CPU spot (%u,%u) distance=%.3f relative=%.6g\n",xy[0],xy[1],length,error);
        require(error<=.03,"CPU finite segment composition differs");
    }
}
void numeric(Fixture& f,SubmissionQueue& q) {
    EnvironmentDescription e; e.enabled=true; e.sun.direction={.3,.15,-1}; e.sun.irradiance={2,1,.5};
    struct Case { double h,d,n,far; };
    for(auto c : {Case{100,10,1,1000},Case{10000,5000,10,50000},Case{10000,50000,100,100000},Case{100000,50000,100,100000},Case{150000,50000,100,100000}}) {
        f.anchor.y()=c.h; f.distance=c.d; f.near_m=c.n; f.far_m=c.far; f.place(); auto sky=f.prepare(q,e);
        f.air.reference=false; const auto actual=f.render(q,sky); f.air.reference=true; const auto reference=f.render(q,sky); f.air.reference=false;
        std::printf("case height=%.0f distance=%.0f near=%.0f far=%.0f\n",c.h,c.d,c.n,c.far);
        compare(linear(actual),linear(reference),"volume/reference"); cpu_spots(f,actual);
        save(actual,"aerial-"+std::to_string(int(c.h))+"-"+std::to_string(int(c.d))+".ppm");
    }
    // Zero solar energy isolates T without using the implementation's own volume as oracle.
    e.sun.irradiance={0,0,0}; f.anchor.y()=10000; f.distance=15000; f.near_m=10; f.far_m=50000; f.place();
    f.asset.materials[0].base_color={1,1,1,1}; f.asset.materials[0].emissive={0,0,0}; f.upload(q);
    auto sky=f.prepare(q,e); const auto transmission=linear(f.render(q,sky)); double maximum=0;
    for(unsigned y=4;y<29;++y) for(unsigned x=4;x<29;++x) {
        const auto direction=ray(f,x,y); const auto ref=take(integrate_atmosphere_reference(f.scene->environment(),f.anchor,direction.normalized(),{1024,8,f.distance*direction.norm(),false}));
        const Vec3d expected=ref.ground && ref.distance_m<f.distance*direction.norm()*.999999 ? Vec3d::Zero().eval() : ref.transmittance;
        for(unsigned c=0;c<3;++c) maximum=std::max(maximum,std::abs(transmission[y*33+x][c]-expected[c]));
    }
    std::printf("RGB transmission max absolute=%.7g\n",maximum); require(maximum<=.002,"RGB T threshold");
    // Changing near/far cannot change the camera-to-surface optical segment.
    f.near_m=100; f.far_m=100000; const auto shifted=linear(f.render(q,sky)); double projection_error=0;
    for(unsigned y=4;y<29;++y) for(unsigned x=4;x<29;++x) for(unsigned c=0;c<3;++c)
        projection_error=std::max(projection_error,double(std::abs(transmission[y*33+x][c]-shifted[y*33+x][c])));
    std::printf("near/far change max absolute=%.7g\n",projection_error); require(projection_error<=.002,"projection changed optical distance");
    f.offset_x=.12; f.offset_y=-.08; cpu_spots(f,f.render(q,sky));
    f.rotation=Eigen::AngleAxisd(.2,Vec3d::UnitX()); f.place();
    cpu_spots(f,f.render(q,sky));
}
void composition(Fixture& f,SubmissionQueue& q) {
    auto e=vacuum(); auto sky=f.prepare(q,e); const auto vacuum_frame=f.render(q,sky);
    expect(vacuum_frame,Vec3d{.81,.52,.28});
    const auto vacuum_hdr=linear(vacuum_frame); require(std::abs(vacuum_hdr[16*33+16][0]-.81)<1e-6,"vacuum surface changed");
    require(vacuum_hdr[0][0]==0 && vacuum_hdr[0][1]==0 && vacuum_hdr[0][2]==0,"vacuum sky nonzero");
    e.enabled=false; sky=f.prepare(q,e); auto old=take(f.pipeline.render(q,f.view(),f.assets,f.settings)); require(take(old.wait(q)),"unlit wait");
    require(pixels(old)==pixels(f.render(q,AtmosphereSky{})),"disabled differs from old unlit");
    // Foreground alpha mask with a separately colored far receiver behind its holes.
    e=EnvironmentDescription{}; e.enabled=true; e.sun.direction={0,.3,-1}; e.sun.irradiance={2,1,.5};
    TextureData texture; texture.id=take(AssetId::generate()); texture.width=2; texture.height=1;
    texture.rgba8={std::byte{255},std::byte{255},std::byte{255},std::byte{0},std::byte{255},std::byte{255},std::byte{255},std::byte{255}};
    texture.sampler.mag_filter=TextureFilter::nearest; texture.sampler.min_filter=TextureFilter::nearest;
    f.asset.textures.push_back(texture); f.asset.materials[0].base_color_texture=texture.id; f.asset.materials[0].alpha_mode=AlphaMode::mask;
    auto back=f.asset.mesh.primitives[0];
    for(auto& vertex:back.positions) { vertex.x()*=.6f; vertex.y()*=.6f; vertex.z()=-1; }
    auto material=f.asset.materials[0]; material.id=take(AssetId::generate()); material.base_color_texture.reset(); material.alpha_mode=AlphaMode::opaque;
    material.base_color={.1f,.7f,.3f,1}; back.material=material.id;
    f.asset.materials.push_back(material); f.asset.mesh.primitives.push_back(back); f.upload(q);
    sky=f.prepare(q,vacuum()); const auto masked_vacuum=f.render(q,sky);
    expect(masked_vacuum,Vec3d{0,0,0},1,6,16); // Hole with sky behind.
    expect(masked_vacuum,Vec3d{.11,.72,.33},1,14,16); // Hole exposes the farther receiver.
    expect(masked_vacuum,Vec3d{.81,.52,.28},1,22,16); // Opaque half hides the receiver/sky.
    sky=f.prepare(q,e,true); const auto actual=f.render(q,sky); f.air.reference=true; const auto ref=f.render(q,sky); f.air.reference=false;
    compare(linear(actual),linear(ref),"mask/near/far/MS"); save(actual,"aerial-mask.ppm");
    const auto hdr=linear(actual); f.settings.exposure=3; const auto exposed=f.render(q,sky);
    require(linear(exposed)==hdr,"exposure changed physical HDR");
    const auto rgba=pixels(exposed);
    for(std::size_t i=0;i<hdr.size();++i) for(unsigned c=0;c<3;++c)
        require(std::abs(std::to_integer<int>(rgba[i*4+c])-encoded(hdr[i][c],3))<=1,"exposure applied more than once");
    // Lit geometry plus AP; compare to independent finite transport of the lit-only surface.
    f.settings.exposure=1; f.air.surface_lighting=true; f.asset.mesh.primitives.resize(1);
    f.asset.materials[0].alpha_mode=AlphaMode::opaque; f.asset.materials[0].base_color_texture.reset(); f.upload(q);
    sky=f.prepare(q,e,false);
    auto lit=take(f.pipeline.render_sunlight(q,f.view(),f.assets,sky,f.settings)); require(take(lit.wait(q)),"lit-only wait");
    const auto surfaces=linear(lit),combined=linear(f.render(q,sky));
    const auto transport=take(integrate_atmosphere_reference(f.scene->environment(),f.anchor,Vec3d{0,0,-1},{1024,1024,f.distance,false}));
    for(unsigned c=0;c<3;++c) require(std::abs(combined[544][c]-(transport.radiance[c]+transport.transmittance[c]*surfaces[544][c]))<.002,"lit composition repeated air or sun attenuation");
    // Solar disk is a background boundary source and is hidden by opaque depth.
    f.air.surface_lighting=false; f.air.sun_disk=true; e=vacuum(); e.sun.direction={0,0,-1};
    sky=f.prepare(q,e); const auto occluded=linear(f.render(q,sky)); require(occluded[544][0]<1,"disk leaked through geometry");
    f.asset.materials[0].alpha_mode=AlphaMode::mask; f.asset.materials[0].base_color.w()=0; f.upload(q);
    const auto disk=linear(f.render(q,sky)); require(disk[544][0]>1000,"mask did not expose solar disk");
    f.air.sun_disk=false; const auto no_disk=linear(f.render(q,sky)); require(no_disk[544][0]==0,"disk toggle changed vacuum sky");
    // The scene background ray and the standalone sky renderer share the same convention.
    e=EnvironmentDescription{}; e.enabled=true; sky=f.prepare(q,e,true);
    SkyCamera camera; camera.width=camera.height=33; camera.vertical_fov_rad=std::numbers::pi/2; camera.sun_disk=false;
    auto standalone=take(f.atmosphere.render_sky(q,sky,camera)); require(take(standalone.wait(q)),"standalone sky wait");
    std::vector<Pixel> sky_pixels(33*33); check(standalone.read_hdr(sky_pixels)); compare(linear(f.render(q,sky)),sky_pixels,"background/standalone");
    auto settings=f.settings; settings.linear_readback=false;
    auto no_read=take(f.pipeline.render_atmosphere(q,f.view(),f.assets,sky,settings,f.air)); require(take(no_read.wait(q)),"no read wait");
    std::vector<Pixel> sentinel(33*33,Pixel{42,42,42,42}),destination=sentinel;
    require(!no_read.read_linear_rgba32f(destination) && destination==sentinel,"unrequested diagnostic read changed output");
    std::puts("vacuum, disabled, mask near/far, MS, HDR/exposure, lit-once, solar occlusion and sky parity passed");
}
void failures(Fixture& f,SubmissionQueue& q,const DeviceOptions& options,memory::MemorySystem& system) {
    auto e=vacuum(); auto sky=f.prepare(q,e); auto view=f.view(); auto first=f.render(q,sky); const auto baseline=pixels(first); const auto generations=sky.generations();
    const auto submitted=q.stats().submitted; const auto base=allocations(q);
    auto unchanged=[&]{require(q.stats().submitted==submitted && allocations(q)==base && pixels(first)==baseline && f.atmosphere.generations()==generations,"failure changed old frame, cache or allocations");};
    auto mismatch=e; mismatch.sun.irradiance[0]*=2; check(f.scene->set_environment(mismatch));
    require(!f.pipeline.render_atmosphere(q,f.view(),f.assets,sky,f.settings,f.air),"mismatched environment accepted"); unchanged(); check(f.scene->set_environment(e));
    require(!f.pipeline.render_atmosphere(q,view,f.assets,AtmosphereSky{}),"empty sky accepted"); unchanged();
    auto shifted=view.description(); Trsd camera; camera.translation=f.anchor+Vec3d{1,0,0}; shifted.camera_world=take(Transformd::from_trs(camera));
    require(!f.pipeline.render_atmosphere(q,take(RenderView::create(view.scene(),shifted)),f.assets,sky,f.settings,f.air),"wrong anchor accepted"); unchanged();
    auto invalid=view.description(); invalid.projection=Mat4d::Identity();
    require(!f.pipeline.render_atmosphere(q,take(RenderView::create(view.scene(),invalid)),f.assets,sky,f.settings,f.air),"orthographic accepted");
    graphics::detail::ObjectAccess::fail_creation(q.resources(),VK_ERROR_OUT_OF_HOST_MEMORY);
    require(!f.pipeline.render_atmosphere(q,view,f.assets,sky,f.settings,f.air),"object failure ignored"); unchanged();
    fail_submit=true; require(!f.pipeline.render_atmosphere(q,view,f.assets,sky,f.settings,f.air),"submit failure ignored"); unchanged();
    auto other=take(SubmissionQueue::create(f.heap,take(Device::create(f.heap,options))));
    require(!f.pipeline.render_atmosphere(other,view,f.assets,sky,f.settings,f.air),"foreign queue accepted"); unchanged();
    {
        auto foreign_renderer=take(AtmosphereRenderer::create(f.heap,other,DK_ATMOSPHERE_SHADER_DIR));
        auto foreign_sky=take(foreign_renderer.prepare(other,e,f.anchor));
        require(!f.pipeline.render_atmosphere(q,view,f.assets,foreign_sky,f.settings,f.air),"foreign LUT accepted"); unchanged();
    }
    check(other.close());
    auto limited=take(system.create_heap({"aerial-budget",memory::DomainCategory::render,1048576}));
    {
        auto pipeline=take(ScenePipeline::create(limited,q,DK_RENDER_SHADER_DIR));
        auto warm=take(pipeline.render_atmosphere(q,view,f.assets,sky,f.settings,f.air)); require(take(warm.wait(q)),"budget warmup"); warm={};
        const auto count=allocations(q);
        struct Block{void* p;std::size_t size;}; std::vector<Block> blocks;
        while(auto b=limited.try_allocate(8192)) blocks.push_back({*b,8192});
        while(auto b=limited.try_allocate(1)) blocks.push_back({*b,1});
        bool failed=false,partial=false,success=false;
        do {
            const auto before=limited.snapshot(); const auto submissions=q.stats().submitted;
            {
                auto frame=pipeline.render_atmosphere(q,view,f.assets,sky,f.settings,f.air);
                if(!frame) {
                    failed=true; partial |= limited.snapshot().allocation_count>before.allocation_count+8;
                    require(limited.snapshot().live_allocations==before.live_allocations && allocations(q)==count && q.stats().submitted==submissions,"budget failure leaked or submitted");
                } else {success=true; require(take(frame->wait(q)),"budget frame wait");}
            }
            if(success || blocks.empty()) break;
            auto b=blocks.back(); blocks.pop_back(); limited.deallocate(b.p,b.size);
        } while(true);
        for(auto b:blocks) limited.deallocate(b.p,b.size);
        require(failed && partial && success,"budget sweep missed partial failure or recovery");
        auto closing=take(pipeline.render_atmosphere(q,view,f.assets,sky,f.settings,f.air)); limited.begin_close();
        require(!pipeline.render_atmosphere(q,view,f.assets,sky,f.settings,f.air),"closing heap accepted new frame");
        require(take(closing.wait(q)),"closing heap broke old frame"); require(pixels(closing)==baseline,"closing frame changed");
    }
    require(limited.snapshot().live_allocations==0,"aerial metadata leaked after closing");
    auto pending=take(f.pipeline.render_atmosphere(q,view,f.assets,sky,f.settings,f.air));
    auto untouched=std::vector<std::byte>(33*33*4,std::byte{42}); const auto sentinel=untouched;
    require(!pending.read_rgba8(untouched) && untouched==sentinel,"pending read changed output");
    std::vector<Pixel> linear_sentinel(33*33,Pixel{42,42,42,42}),linear_destination=linear_sentinel;
    require(!pending.read_linear_rgba32f(linear_destination) && linear_destination==linear_sentinel,"pending linear read changed output");
    require(!first.read_linear_rgba32f(std::span{linear_destination}.first(1)) && linear_destination==linear_sentinel,"wrong size linear read changed output");
    auto newer=f.prepare(q,mismatch); require(pixels(first)==baseline,"new sky changed old image");
    require(take(pending.wait(q)),"cache-hit prepare does not wait for pending frame");
    check(pending.read_rgba8(untouched)); require(untouched==baseline,"in-flight frame used new environment");
    auto last=take(f.pipeline.render_atmosphere(q,f.view(),f.assets,newer,f.settings,f.air));
    f.assets.clear(); f.pipeline={}; f.atmosphere={}; sky={}; newer={};
    require(take(last.wait(q)),"owner destruction broke pending frame");
    require(linear(last)[544][0]>.8f,"destroyed owners lost surface");
    std::puts("snapshot matching, empty/anchor/projection/object/submit/foreign/budget failures, closing, pending read and old owners passed");
}
}
int main(int argc,char** argv) {
    Diagnostics d;
    try {
        require(argc==2,"expected numeric/composition/failures"); const std::string mode=argv[1];
        auto system=take(memory::MemorySystem::create()); auto heap=take(system.create_heap({"aerial-perspective",memory::DomainCategory::render}));
        {
            memory::ThreadContext context{system}; memory::ExecutionScope scope{context,heap};
            DeviceOptions options; options.validation=ValidationMode::required; options.diagnostic_sink=diagnostic; options.diagnostic_user_data=&d;
            auto device=take(Device::create(heap,options)); native_submit=reinterpret_cast<PFN_vkQueueSubmit2>(device.device_proc("vkQueueSubmit2"));
            auto q=take(graphics::detail::SubmissionAccess::create(heap,std::move(device),3,{submit}));
            std::printf("GPU=%s driver=%s\n",q.device().adapter().properties.deviceName.data(),q.device().adapter().driver.driverInfo.data());
            { Fixture f{heap,q};
                if(mode=="numeric") numeric(f,q); else if(mode=="composition") composition(f,q);
                else if(mode=="failures") failures(f,q,options,system); else throw std::runtime_error("unknown mode");
            }
            check(q.close()); require(allocations(q)==0,"GPU allocation leak");
        }
        std::printf("aerial comparisons=%u max_byte_error=%u errors=%u warnings=%u live=%zu\n",comparisons,max_byte_error,d.errors.load(),d.warnings.load(),heap.snapshot().live_allocations);
        return d.errors==0 && d.warnings==0 && heap.snapshot().live_allocations==0 && system.try_close().closed() ? 0 : 1;
    } catch(const std::exception& error) { std::fprintf(stderr,"aerial probe failed: %s\n",error.what()); return 1; }
}

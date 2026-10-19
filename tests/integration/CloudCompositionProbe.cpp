#include "../RenderAssetSupport.hpp"
#include <dk/render/ScenePipeline.hpp>
#include <dk/math/Projection.hpp>
#include <dk/render/Atmosphere.hpp>
#include <dk/render/CloudReference.hpp>
#include <dk/render/CloudTextures.hpp>
#include <dk/render/CloudShadowRenderer.hpp>
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
    CloudFieldCache fields; std::shared_ptr<const CloudFieldData> data;
    std::unique_ptr<CloudTextureCache> textures; std::shared_ptr<const CloudTextures> texture;
    CloudCompositionSettings clouds; unsigned side=33;
    Vec3d anchor{0,10000,0}; double distance=5000,near_m=10,far_m=50000;
    double offset_x=0,offset_y=0,ortho_height=0;
    Quatd rotation=Quatd::Identity();
    RenderSettings settings{{.01f,.02f,.03f},1,true,false,true};
    AtmosphereSettings air{false,false,false};
    Fixture(memory::ResourceHandle h,SubmissionQueue& q):heap(h),assets(take(GpuAssets::create(h))),
        pipeline(take(ScenePipeline::create(h,q,DK_RENDER_SHADER_DIR))),
        atmosphere(take(AtmosphereRenderer::create(h,q,DK_ATMOSPHERE_SHADER_DIR))) {
        clouds.surface_lighting=false; clouds.sun_disk=false; clouds.diagnostics=true;
        textures=take(CloudTextureCache::create(q));
        asset.mesh.id=take(AssetId::generate());
        MeshPrimitive p; p.positions={{-.8f,-.8f,0},{.8f,-.8f,0},{.8f,.8f,0},{-.8f,.8f,0}};
        p.normals.assign(4,Vec3f{0,0,1}); p.texcoords={{0,0},{1,0},{1,1},{0,1}}; p.indices={0,1,2,0,2,3};
        MaterialData m; m.id=take(AssetId::generate()); m.base_color={.8f,.5f,.25f,1}; m.emissive={.01f,.02f,.03f}; m.double_sided=true;
        p.material=m.id; asset.materials.push_back(m); asset.mesh.primitives.push_back(p);
        check(scene->create_entity(entity)); check(scene->set_asset_references(entity,{{asset.mesh.id,AssetKind::mesh}})); place(); upload(q);
    }
    void place() { Trsd t; t.translation=anchor+rotation*Vec3d{0,0,-distance}; t.rotation=rotation; t.scale=Vec3d::Constant(ortho_height>0 ? ortho_height*.5 : distance); check(scene->set_local_transform(entity,t)); }
    void upload(SubmissionQueue& q) { auto a=take(assets.upload(q,asset)); require(take(a.wait(q)),"upload wait"); }
    RenderView view() {
        ViewDescription d{side,side}; Trsd camera; camera.translation=anchor; camera.rotation=rotation;
        d.camera_world=take(Transformd::from_trs(camera)); d.projection=Mat4d::Zero();
        d.projection(0,0)=1; d.projection(1,1)=-1; d.projection(0,2)=offset_x; d.projection(1,2)=offset_y;
        d.projection(2,2)=far_m/(near_m-far_m); d.projection(2,3)=near_m*far_m/(near_m-far_m); d.projection(3,2)=-1;
        if(ortho_height>0) d.projection=take(orthographic_projection({ortho_height,near_m,far_m},1));
        return take(RenderView::create(take(RenderScene::extract(heap,take(scene->snapshot()))),d));
    }
    AtmosphereSky prepare(SubmissionQueue& q,EnvironmentDescription e,bool multiple=false) {
        check(scene->set_environment(e)); data=take(fields.prepare(e.clouds)); texture=take(textures->prepare(q,*data));
        return take(atmosphere.prepare(q,scene->environment(),anchor,multiple));
    }
    RenderFrame render(SubmissionQueue& q,const AtmosphereSky& sky) {
        auto f=take(pipeline.render_clouds(q,view(),assets,sky,*texture,clouds,settings)); require(take(f.wait(q)),"frame wait"); return f;
    }
};
using Pixel=std::array<float,4>;
std::vector<Pixel> linear(const RenderFrame& f) {
    std::vector<Pixel> p(std::size_t(f.info().width)*f.info().height); check(f.read_linear_rgba32f(p)); return p;
}
std::vector<std::byte> pixels(const RenderFrame& f) {
    std::vector<std::byte> p(std::size_t(f.info().width)*f.info().height*4); check(f.read_rgba8(p)); return p;
}
int encoded(double x,double exposure) {
    x=x*exposure/(1+x*exposure); return int(std::lround(255*(x<=.0031308 ? 12.92*x : 1.055*std::pow(x,1/2.4)-.055)));
}
void save(const RenderFrame& f,const std::string& name) {
    const auto p=pixels(f); std::ofstream out{name,std::ios::binary}; out<<"P6\n"<<f.info().width<<' '<<f.info().height<<"\n255\n";
    for(std::size_t i=0;i<p.size();i+=4) out.write(reinterpret_cast<const char*>(p.data()+i),3);
    require(bool(out),"sample write failed");
}

Vec3d ray(const Fixture& f,unsigned x,unsigned y) {
    if(f.ortho_height>0) return f.rotation*Vec3d{0,0,-1};
    return f.rotation*Vec3d{(x+.5)/f.side*2-1+f.offset_x,-((y+.5)/f.side*2-1+f.offset_y),-1};
}
Vec3d origin(const Fixture& f,unsigned x,unsigned y) {
    return f.ortho_height>0 ? (f.anchor+f.rotation*Vec3d{((x+.5)/f.side-.5)*f.ortho_height,(.5-(y+.5)/f.side)*f.ortho_height,-f.near_m}).eval() : f.anchor;
}
void compare(const std::vector<Pixel>& actual,const std::vector<Pixel>& expected,const char* name,double rms_limit=.03,double tail_limit=.08) {
    std::vector<double> luminance,errors; double squares=0,maximum=0,edge=0;
    for(const auto& pixel:expected) luminance.push_back(.2126*pixel[0]+.7152*pixel[1]+.0722*pixel[2]);
    std::ranges::sort(luminance); const double scale=std::max(.001,luminance[std::size_t(.95*(luminance.size()-1))]);
    for(std::size_t i=0;i<actual.size();++i) {
        require(actual[i][3]==1,"linear alpha not one");
        for(unsigned c=0;c<3;++c) {
            require(std::isfinite(actual[i][c]) && actual[i][c]>=0,"invalid radiance");
            const double error=std::abs(actual[i][c]-expected[i][c])/scale;
            errors.push_back(error); squares+=error*error; maximum=std::max(maximum,error);
            const auto side=unsigned(std::sqrt(actual.size())); const auto x=i%side,y=i/side;
            if(x==2 || x==side-3 || y==2 || y==side-3 || y==side/2) edge=std::max(edge,error);
        }
    }
    std::ranges::sort(errors); const double rmse=std::sqrt(squares/errors.size()),p99=errors[std::size_t(.99*(errors.size()-1))];
    std::printf("%s scale=%.6g nRMSE=%.6g p99=%.6g max=%.6g edge/horizon=%.6g\n",name,scale,rmse,p99,maximum,edge);
    require(rmse<=rms_limit && p99<=tail_limit,"linear radiance reference threshold");
    require(edge<=tail_limit,"edge/horizon reference threshold");
}
std::vector<Pixel> diagnostic_pixels(const RenderFrame& frame,bool medium) {
    std::vector<Pixel> out(std::size_t(frame.info().width)*frame.info().height);
    check(medium ? frame.read_medium_rgba32f(out) : frame.read_cloud_rgba32f(out)); return out;
}
void save_diagnostics(const RenderFrame& frame) {
    const auto cloud=diagnostic_pixels(frame,false),medium=diagnostic_pixels(frame,true);
    for(unsigned kind=0;kind<3;++kind) {
        std::ofstream out{kind==0 ? "cloud-transmission.ppm" : kind==1 ? "medium-transmission.ppm" : "cloud-distance.ppm",std::ios::binary};
        out<<"P6\n"<<frame.info().width<<' '<<frame.info().height<<"\n255\n";
        for(std::size_t i=0;i<cloud.size();++i) for(unsigned c=0;c<3;++c) {
            const float v=kind==0 ? cloud[i][3] : kind==1 ? medium[i][c] : medium[i][3]/40000;
            const auto byte=static_cast<char>(std::lround(255*std::clamp(v,0.f,1.f))); out.write(&byte,1);
        }
        require(bool(out),"diagnostic image write failed");
    }
}
EnvironmentDescription cloudy() {
    EnvironmentDescription e; e.enabled=true; e.clouds.enabled=true; e.clouds.coverage=.85;
    e.sun.direction={.4,.5,-.6}; return take(normalize_environment(e));
}
void cpu_spots(Fixture& f,const RenderFrame& frame) {
    const auto hdr=linear(frame),cloud=diagnostic_pixels(frame,false),medium=diagnostic_pixels(frame,true);
    const unsigned mid=f.side/2;
    for(auto point:{std::array<unsigned,2>{mid,mid},{3,mid},{f.side-4,mid},{mid,3},{mid,f.side-4}}) {
        const auto direction=ray(f,point[0],point[1]); const double distance=f.distance*direction.norm();
        const auto value=take(integrate_cloud_medium_reference(f.scene->environment(),*f.data,{f.anchor,direction,distance},f.clouds.reference));
        const Vec3d surface=(f.asset.materials[0].base_color.head<3>()+f.asset.materials[0].emissive).cast<double>();
        const Vec3d expected=value.radiance+(value.ground ? Vec3d::Zero().eval() : value.transmittance.cwiseProduct(surface));
        const auto index=point[1]*f.side+point[0]; const Vec3d actual{hdr[index][0],hdr[index][1],hdr[index][2]};
        const double rgb=(actual-expected).cwiseAbs().maxCoeff()/std::max(.001,expected.maxCoeff());
        const double trans=std::abs(cloud[index][3]-value.cloud_transmittance);
        std::printf("joint CPU spot (%u,%u) RGB=%.9g cloud_T=%.9g\n",point[0],point[1],rgb,trans);
        require(rgb<=.03 && trans<=.005,"joint CPU reference differs");
        for(unsigned c=0;c<3;++c) require(std::abs(medium[index][c]-value.transmittance[c])<=.005,"mixed T differs from CPU");
    }
}
void cpu_image(Fixture& f,const RenderFrame& frame,const char* name) {
    const auto cloud=diagnostic_pixels(frame,false),medium=diagnostic_pixels(frame,true);
    std::vector<Pixel> expected; double cloud_error=0,medium_error=0;
    const Vec3d surface=(f.asset.materials[0].base_color.head<3>()+f.asset.materials[0].emissive).cast<double>();
    for(unsigned y=0;y<f.side;++y) for(unsigned x=0;x<f.side;++x) {
        const auto direction=ray(f,x,y);
        const bool geometry=std::abs((x+.5)/f.side*2-1+f.offset_x)<.8 && std::abs((y+.5)/f.side*2-1+f.offset_y)<.8;
        const auto value=take(integrate_cloud_medium_reference(f.scene->environment(),*f.data,
            {origin(f,x,y),direction,geometry ? (f.distance-(f.ortho_height>0 ? f.near_m : 0))*direction.norm() : 2e7},f.clouds.reference));
        Vec3d rgb=value.radiance;
        if(geometry && !value.ground) rgb+=value.transmittance.cwiseProduct(surface);
        expected.push_back({float(rgb.x()),float(rgb.y()),float(rgb.z()),1});
        const auto index=y*f.side+x;
        cloud_error=std::max(cloud_error,std::abs(cloud[index][3]-value.cloud_transmittance));
        for(unsigned c=0;c<3;++c) medium_error=std::max(medium_error,std::abs(medium[index][c]-value.transmittance[c]));
    }
    compare(linear(frame),expected,name);
    std::printf("%s cloud_T=%.9g mixed_T=%.9g\n",name,cloud_error,medium_error);
    require(cloud_error<=.005 && medium_error<=.005,"full CPU image transmission differs");
}
void numeric(Fixture& f,SubmissionQueue& q) {
    f.side=15; f.anchor={0,2200,0}; f.distance=3000; f.place(); auto e=cloudy();
    f.clouds.reference.time_seconds=17.25; f.clouds.reference.sky_radiance={.015,.02,.03};
    const auto sky=f.prepare(q,e); const auto low_frame=f.render(q,sky); const auto low=linear(low_frame); cpu_spots(f,low_frame);
    const auto original=f.clouds.reference;
    f.clouds.reference.steps=512; f.clouds.reference.sun_steps=f.clouds.reference.atmosphere_steps=128;
    const auto middle=linear(f.render(q,sky));
    f.clouds.reference.steps=1024; f.clouds.reference.sun_steps=f.clouds.reference.atmosphere_steps=256;
    const auto high=linear(f.render(q,sky)); compare(low,middle,"joint 256-to-512"); compare(middle,high,"joint 512-to-1024");
    f.clouds.reference=original;
    const auto first=f.render(q,sky); save(first,"cloud-composition.ppm"); save_diagnostics(first);
    f.clouds.reference.time_seconds=-400; const auto moved=linear(f.render(q,sky)); require(moved!=low,"explicit wind time did not change image");
    f.clouds.reference.time_seconds=17.25; require(linear(f.render(q,sky))==low,"time restore accumulated history");
    // Cloud-only radiance is attenuated by the intervening/overlapping air.
    e.sun.irradiance={0,0,0}; auto dark_sky=f.prepare(q,e); const auto with_air=diagnostic_pixels(f.render(q,dark_sky),false);
    e.enabled=false; auto no_air_sky=f.prepare(q,e); const auto no_air=diagnostic_pixels(f.render(q,no_air_sky),false);
    bool dimmer=false;
    for(std::size_t i=0;i<no_air.size();++i) for(unsigned c=0;c<3;++c) {
        require(with_air[i][c]<=no_air[i][c]+1e-5,"air increased isolated cloud source"); dimmer|=with_air[i][c]<no_air[i][c]-.0001;
    }
    require(dimmer,"far cloud did not receive air attenuation");
    // Include background, spherical ground and the horizon on both sides of the cloud shell.
    f.side=9; f.distance=6000;
    for(unsigned mode=0;mode<3;++mode) {
        f.anchor.y()=mode==0 ? 100 : mode==1 ? 6000 : 2200; f.place(); e=cloudy();
        if(mode==2) { e.sun.direction={0,.02,-1}; e=take(normalize_environment(e)); }
        const auto case_sky=f.prepare(q,e); const auto image=f.render(q,case_sky);
        const char* name=mode==0 ? "cloud-below" : mode==1 ? "cloud-above" : "cloud-sunset";
        cpu_image(f,image,name); save(image,std::string{name}+".ppm");
    }
    std::puts("joint convergence, CPU independent reference, explicit wind and cloud air perspective passed");
}
void composition(Fixture& f,SubmissionQueue& q) {
    f.anchor={0,100,0}; f.rotation=Quatd::FromTwoVectors(Vec3d{0,0,-1},Vec3d{0,1,0}); f.distance=5000; f.place();
    auto e=cloudy(); e.sun.irradiance={0,0,0};
    e.atmosphere.rayleigh_scattering_per_m={}; e.atmosphere.mie_scattering_per_m={}; e.atmosphere.mie_extinction_per_m={};
    e.atmosphere.absorption_center_height_m=0; e.atmosphere.absorption_half_width_m=10000; e.atmosphere.absorption_extinction_per_m={1e-4,2e-4,3e-4};
    e.clouds.extinction_per_m=.0003; f.clouds.reference.homogeneous=true; f.clouds.reference.single_scattering_albedo=0;
    auto sky=f.prepare(q,e); auto frame=f.render(q,sky); const auto medium=diagnostic_pixels(frame,true),cloud=diagnostic_pixels(frame,false);
    const double air_depth=f.distance-(2*f.anchor.y()*f.distance+f.distance*f.distance)/20000;
    const Vec3d surface{.81,.52,.28};
    for(unsigned c=0;c<3;++c) {
        const double trans=std::exp(-air_depth*e.atmosphere.absorption_extinction_per_m[c]-.0003*2500);
        require(std::abs(medium[544][c]-trans)<=.002,"mixed analytic transmission");
        require(std::abs(linear(frame)[544][c]-trans*surface[c])<=.002,"surface or air attenuated twice");
    }
    require(std::abs(cloud[544][3]-std::exp(-.75))<=.002,"cloud-only diagnostic T");
    // Geometry ending before cloud entry must receive no cloud extinction; mask holes keep tracing.
    f.distance=1000; f.place(); frame=f.render(q,sky); require(diagnostic_pixels(frame,false)[544][3]==1,"foreground leaked clouds");
    TextureData texture; texture.id=take(AssetId::generate()); texture.width=2; texture.height=1;
    texture.rgba8={std::byte{255},std::byte{255},std::byte{255},std::byte{0},std::byte{255},std::byte{255},std::byte{255},std::byte{255}};
    texture.sampler.mag_filter=texture.sampler.min_filter=TextureFilter::nearest;
    f.asset.textures.push_back(texture); f.asset.materials[0].base_color_texture=texture.id; f.asset.materials[0].alpha_mode=AlphaMode::mask; f.upload(q);
    frame=f.render(q,sky); const auto mask=diagnostic_pixels(frame,false);
    require(mask[16*33+6][3]<.8 && mask[16*33+22][3]==1,"alpha mask depth failed cloud clipping"); save(frame,"cloud-mask.ppm");
    // A solar disk is attenuated once by total T, and never leaks through an opaque foreground.
    e=vacuum(); e.clouds.enabled=true; e.clouds.extinction_per_m=.0003; e.sun.direction={0,1,0}; e.sun.irradiance={1,1,1};
    f.clouds.sun_disk=true; f.asset.materials[0].base_color.w()=0; f.upload(q); sky=f.prepare(q,e); frame=f.render(q,sky);
    const auto transmission=diagnostic_pixels(frame,true); const auto solar=take(solar_disk_radiance(e.sun));
    for(unsigned c=0;c<3;++c) require(std::abs(linear(frame)[544][c]/solar[c]-transmission[544][c])<=.002,"sun cloud occlusion repeated or omitted T");
    require(transmission[544][0]<.6,"cloud did not occlude solar disk");
    f.asset.materials[0].base_color.w()=1; f.asset.materials[0].alpha_mode=AlphaMode::opaque; f.upload(q);
    require(linear(f.render(q,sky))[544][0]<1,"sun leaked foreground");
    // Zero controls select the exact old M14 implementation, including its sky/AP discretization.
    f.clouds.sun_disk=false;
    for(unsigned mode=0;mode<4;++mode) {
        auto zero=cloudy(); if(mode==0) zero.clouds.enabled=false; if(mode==1) zero.clouds.coverage=0;
        if(mode==2) zero.clouds.density=0; if(mode==3) zero.clouds.extinction_per_m=0;
        sky=f.prepare(q,zero); auto old=take(f.pipeline.render_atmosphere(q,f.view(),f.assets,sky,f.settings,{false,false,false})); require(take(old.wait(q)),"old M14 wait");
        auto fallback=f.render(q,sky); require(pixels(old)==pixels(fallback) && linear(old)==linear(fallback),"zero cloud did not exactly degenerate");
    }
    e=cloudy(); sky=f.prepare(q,e,true); f.clouds.reference=CloudReferenceSettings{}; f.distance=6000; f.place();
    const auto unexposed=f.render(q,sky); const auto hdr=linear(unexposed); f.settings.exposure=3;
    const auto exposed=f.render(q,sky); require(linear(exposed)==hdr,"exposure affected physical mixed HDR");
    const auto rgba=pixels(exposed); for(std::size_t i=0;i<hdr.size();++i) for(unsigned c=0;c<3;++c)
        require(std::abs(std::to_integer<int>(rgba[4*i+c])-encoded(hdr[i][c],3))<=1,"mixed exposure/tone applied twice");
    f.settings.exposure=1;
    // The shared environment entry must also render nonzero clouds, with the same default model.
    f.clouds.surface_lighting=true; f.clouds.sun_disk=true;
    EnvironmentRenderSettings common_settings; common_settings.cloud_shadows=false; // Match the explicit unshadowed composition.
    const auto explicit_frame=f.render(q,sky); auto common=take(f.pipeline.render_environment(q,f.view(),f.assets,f.settings,common_settings)); require(take(common.wait(q)),"common environment wait");
    require(linear(common)==linear(explicit_frame),"common environment diverged from explicit cloud composition");
    require(common.plan_text().find("joint cloud air geometry composition")!=std::string_view::npos && common.plan_text().find("aerial perspective volume")==std::string_view::npos,"cloud path still applied full AP");
    std::puts("mixed analytic, foreground/mask, solar boundary, zero M14, single exposure and common environment passed");
}
void failures(Fixture& f,SubmissionQueue& q,const DeviceOptions& options,memory::MemorySystem& system) {
    f.anchor={0,2200,0}; f.distance=3000; f.place(); auto e=cloudy(); auto sky=f.prepare(q,e); const auto view=f.view();
    auto first=f.render(q,sky); const auto baseline=linear(first),diagnostic=diagnostic_pixels(first,false); const auto base=allocations(q); const auto submitted=q.stats().submitted;
    auto unchanged=[&] { require(allocations(q)==base && q.stats().submitted==submitted && linear(first)==baseline && diagnostic_pixels(first,false)==diagnostic,"cloud failure changed old frame or leaked"); };
    auto bad=f.clouds; bad.max_work=1; require(!f.pipeline.render_clouds(q,view,f.assets,sky,*f.texture,bad,f.settings),"budget accepted"); unchanged();
    auto desc=view.description(); desc.width=desc.height=65;
    require(!f.pipeline.render_clouds(q,take(RenderView::create(view.scene(),desc)),f.assets,sky,*f.texture,f.clouds,f.settings),"reference extent accepted"); unchanged();
    desc=view.description(); desc.projection=Mat4d::Identity(); require(!f.pipeline.render_clouds(q,take(RenderView::create(view.scene(),desc)),f.assets,sky,*f.texture,f.clouds,f.settings),"invalid identity projection accepted"); unchanged();
    auto mismatch=e; mismatch.sun.irradiance[0]=2; check(f.scene->set_environment(mismatch));
    require(!f.pipeline.render_clouds(q,f.view(),f.assets,sky,*f.texture,f.clouds,f.settings),"mismatched sky accepted"); unchanged(); check(f.scene->set_environment(e));
    graphics::detail::ObjectAccess::fail_creation(q.resources(),VK_ERROR_OUT_OF_HOST_MEMORY);
    require(!f.pipeline.render_clouds(q,view,f.assets,sky,*f.texture,f.clouds,f.settings),"object fault ignored"); unchanged();
    fail_submit=true; require(!f.pipeline.render_clouds(q,view,f.assets,sky,*f.texture,f.clouds,f.settings) && !fail_submit,"submit fault ignored"); unchanged();
    auto other=take(SubmissionQueue::create(f.heap,take(Device::create(f.heap,options))));
    require(!f.pipeline.render_clouds(other,view,f.assets,sky,*f.texture,f.clouds,f.settings),"foreign queue accepted"); unchanged();
    { auto cache=take(CloudTextureCache::create(other)); const auto texture=take(cache->prepare(other,*f.data));
        require(!f.pipeline.render_clouds(q,view,f.assets,sky,*texture,f.clouds,f.settings),"foreign texture accepted"); }
    unchanged(); check(other.close());
    auto pending=take(f.pipeline.render_clouds(q,view,f.assets,sky,*f.texture,f.clouds,f.settings));
    std::vector<Pixel> sentinel(33*33,Pixel{42,42,42,42}),destination=sentinel;
    require(!pending.read_cloud_rgba32f(destination) && destination==sentinel,"pending diagnostic read wrote output");
    require(!first.read_medium_rgba32f(std::span{destination}.first(1)) && destination==sentinel,"wrong diagnostic size wrote output");
    require(take(pending.wait(q)),"pending wait"); require(linear(pending)==baseline,"retry after failure changed image");
    auto no_diag=f.clouds; no_diag.diagnostics=false; auto ordinary=take(f.pipeline.render_clouds(q,view,f.assets,sky,*f.texture,no_diag,f.settings)); require(take(ordinary.wait(q)),"ordinary wait");
    require(!ordinary.read_cloud_rgba32f(destination) && destination==sentinel,"unrequested diagnostic changed output");
    auto closing=take(system.create_heap({"cloud-composition-closing",memory::DomainCategory::render}));
    { auto pipeline=take(ScenePipeline::create(closing,q,DK_RENDER_SHADER_DIR)); auto frame=take(pipeline.render_clouds(q,view,f.assets,sky,*f.texture,f.clouds,f.settings));
        closing.begin_close(); require(!pipeline.render_clouds(q,view,f.assets,sky,*f.texture,f.clouds,f.settings),"closed heap accepted frame");
        pipeline={}; require(take(frame.wait(q)) && linear(frame)==baseline,"closing lost pending frame"); }
    require(closing.snapshot().live_allocations==0,"cloud closing leaked");
    auto last=take(f.pipeline.render_clouds(q,view,f.assets,sky,*f.texture,f.clouds,f.settings));
    f.assets.clear(); f.pipeline={}; f.texture.reset(); f.textures.reset(); f.atmosphere={}; sky={};
    require(take(last.wait(q)) && linear(last)==baseline && diagnostic_pixels(last,false)==diagnostic,"pending owners lost cloud resources");
    std::puts("cloud budgets, snapshot/queue/object/submit, diagnostic read and pending owner lifetime passed");
}
RenderFrame optimized_frame(Fixture& f,SubmissionQueue& q,const AtmosphereSky& sky,const CloudOptimizedSettings& settings) {
    auto result=take(f.pipeline.render_clouds_optimized(q,f.view(),f.assets,sky,*f.texture,settings,f.settings));
    require(take(result.wait(q)),"optimized frame wait"); return result;
}
void optimization(Fixture& f,SubmissionQueue& q) {
    f.side=31; f.anchor={0,2200,0}; f.distance=3000; f.place(); auto e=cloudy();
    CloudOptimizedSettings settings; settings.temporal=false; settings.composition.surface_lighting=false;
    settings.composition.sun_disk=false; settings.composition.diagnostics=true;
    settings.composition.reference.sky_radiance={.015,.02,.03}; settings.composition.reference.time_seconds=17.25;
    f.clouds=settings.composition; f.clouds.reference.steps=512; f.clouds.reference.sun_steps=f.clouds.reference.atmosphere_steps=128;
    const auto sky=f.prepare(q,e); const auto reference=f.render(q,sky); const auto result=optimized_frame(f,q,sky,settings);
    compare(linear(result),linear(reference),"optimized versus 512 reference",.05,.12);
    const auto stats=take(result.read_cloud_statistics());
    std::printf("optimized steps=%u lighting=%u empty=%u early=%u reconstructed=%u fallback=%u\n",stats.primary_steps,stats.cloud_lighting_steps,
        stats.empty_steps,stats.terminated_steps,stats.reconstructed_pixels,stats.fallback_pixels);
    std::printf("primary density samples=%llu\n",stats.density_samples);
    require(stats.density_samples>=stats.cloud_lighting_steps,"density statistics missing");
    require(result.cloud_optimization().width==16 && result.cloud_optimization().height==16 && stats.reconstructed_pixels>0,"half resolution reconstruction missing");
    const auto actual=diagnostic_pixels(result,false),expected=diagnostic_pixels(reference,false);
    double trans=0; std::size_t worst=0;
    for(std::size_t i=0;i<actual.size();++i) if(std::abs(double(actual[i][3])-expected[i][3])>trans) { trans=std::abs(double(actual[i][3])-expected[i][3]); worst=i; }
    std::printf("optimized cloud T max=%.9g at %zu,%zu actual=%.9g expected=%.9g\n",trans,worst%f.side,worst/f.side,actual[worst][3],expected[worst][3]);
    require(trans<=.02,"optimized cloud T reference threshold");
    save(result,"cloud-optimized.ppm");
}
#include "CloudReconstructionCases.hpp"
#include "CloudShadowCases.hpp"
#include "CloudOrthographicCases.hpp"
}
int main(int argc,char** argv) {
    Diagnostics d;
    try {
        require(argc==2,"expected numeric/composition/failures"); const std::string mode=argv[1];
        auto system=take(memory::MemorySystem::create()); auto heap=take(system.create_heap({"cloud-composition",memory::DomainCategory::render}));
        {
            memory::ThreadContext context{system}; memory::ExecutionScope scope{context,heap};
            DeviceOptions options; options.validation=ValidationMode::required; options.diagnostic_sink=diagnostic; options.diagnostic_user_data=&d;
            auto device=take(Device::create(heap,options)); native_submit=reinterpret_cast<PFN_vkQueueSubmit2>(device.device_proc("vkQueueSubmit2"));
            auto q=take(graphics::detail::SubmissionAccess::create(heap,std::move(device),3,{submit}));
            std::printf("GPU=%s\n",q.device().adapter().properties.deviceName.data());
            { Fixture f{heap,q}; if(mode=="numeric") numeric(f,q); else if(mode=="composition") composition(f,q);
                else if(mode=="failures") failures(f,q,options,system); else if(mode=="optimization") optimization(f,q);
                else if(mode=="optimization_boundaries") optimization_boundaries(f,q);
                else if(mode=="optimization_switches") optimization_switches(f,q); else if(mode=="temporal") temporal(f,q);
                else if(mode=="history") history_lifecycle(f,q,options,system);
                else if(mode=="orthographic") orthographic(f,q);
                else if(mode=="shadow_numeric") shadow_numeric(f,q);
                else if(mode=="shadow_surface") shadow_surface(f,q);
                else if(mode=="orthographic_shadow") { f.ortho_height=800; shadow_surface(f,q); }
                else if(mode=="shadow_failures") shadow_failures(f,q,options,system);
                else throw std::runtime_error("unknown mode"); }
            require(!q.stats().pending_slots && !q.stats().recording_slots && allocations(q)==0,"cloud GPU resources leaked"); check(q.close());
        }
        std::printf("cloud composition errors=%u warnings=%u live=%zu\n",d.errors.load(),d.warnings.load(),heap.snapshot().live_allocations);
        return !d.errors && !d.warnings && !heap.snapshot().live_allocations && system.try_close().closed() ? 0 : 1;
    } catch(const std::exception& error) { std::fprintf(stderr,"cloud composition failed: %s\n",error.what()); return 1; }
}

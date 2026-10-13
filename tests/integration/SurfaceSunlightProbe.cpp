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
    EntityId entity=take(EntityId::generate()),parent=take(EntityId::generate());
    GpuAssets assets;
    ScenePipeline pipeline;
    AtmosphereRenderer atmosphere;
    Vec3d anchor{0,100,0};
    RenderSettings settings{{.01f,.02f,.03f},1,true,false};
    Fixture(memory::ResourceHandle h,SubmissionQueue& q):heap(h),assets(take(GpuAssets::create(h))),
        pipeline(take(ScenePipeline::create(h,q,DK_RENDER_SHADER_DIR))),
        atmosphere(take(AtmosphereRenderer::create(h,q,DK_ATMOSPHERE_SHADER_DIR))) {
        asset.mesh.id=take(AssetId::generate());
        MeshPrimitive p; p.positions={{-.8f,-.8f,0},{.8f,-.8f,0},{.8f,.8f,0},{-.8f,.8f,0}};
        p.normals.assign(4,Vec3f{0,0,1}); p.texcoords={{0,0},{1,0},{1,1},{0,1}}; p.indices={0,1,2,0,2,3};
        MaterialData m; m.id=take(AssetId::generate()); m.base_color={.8f,.5f,.25f,1}; m.emissive={.01f,.02f,.03f};
        p.material=m.id; asset.materials.push_back(m); asset.mesh.primitives.push_back(p);
        check(scene->create_entity(parent)); check(scene->create_entity(entity)); check(scene->set_parent(entity,parent));
        check(scene->set_asset_references(entity,{{asset.mesh.id,AssetKind::mesh}})); place(); upload(q);
    }
    void place() { Trsd t; t.translation=anchor; check(scene->set_local_transform(parent,t)); }
    void upload(SubmissionQueue& q) { auto a=take(assets.upload(q,asset)); require(take(a.wait(q)),"upload wait"); }
    RenderView view() {
        ViewDescription d{33,33}; Trsd camera; camera.translation=anchor+Vec3d{0,0,2};
        d.camera_world=take(Transformd::from_trs(camera)); d.projection(1,1)=-1; d.projection(2,2)=-.25;
        return take(RenderView::create(take(RenderScene::extract(heap,take(scene->snapshot()))),d));
    }
    AtmosphereSky prepare(SubmissionQueue& q,EnvironmentDescription e) {
        check(scene->set_environment(e)); return take(atmosphere.prepare(q,scene->environment(),anchor));
    }
    RenderFrame render(SubmissionQueue& q,const AtmosphereSky& sky) {
        auto f=take(pipeline.render_sunlight(q,view(),assets,sky,settings)); require(take(f.wait(q)),"frame wait"); return f;
    }
};
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
Vec3d lambert(const Fixture& f,const EnvironmentDescription& e,const Vec3d& n,const Vec3d& transmission=Vec3d::Ones()) {
    const Vec3d sun{e.sun.direction[0],e.sun.direction[1],e.sun.direction[2]},energy{e.sun.irradiance[0],e.sun.irradiance[1],e.sun.irradiance[2]};
    return f.asset.materials[0].base_color.head<3>().cast<double>().cwiseProduct(energy).cwiseProduct(transmission)*
        (std::max(0.,n.normalized().dot(sun))/std::numbers::pi)+f.asset.materials[0].emissive.cast<double>();
}
void save(const RenderFrame& f,const std::string& name) {
    const auto p=pixels(f); std::ofstream out{name,std::ios::binary}; out<<"P6\n"<<f.info().width<<' '<<f.info().height<<"\n255\n";
    for(std::size_t i=0;i<p.size();i+=4) out.write(reinterpret_cast<const char*>(p.data()+i),3);
    require(bool(out),"sample write failed");
}
void normals(Fixture& f,SubmissionQueue& q) {
    const auto e=vacuum(); auto sky=f.prepare(q,e);
    expect(f.render(q,sky),lambert(f,e,Vec3d::UnitZ()));
    for(Vec3f normal : {Vec3f{.4f,.7f,1},Vec3f{0,0,0},Vec3f{0,0,1e30f}}) {
        f.asset.mesh.primitives[0].normals.assign(4,normal); f.upload(q);
        expect(f.render(q,sky),lambert(f,e,normal.isZero() ? Vec3d{0,0,1} : normal.cast<double>().eval()));
    }
    f.asset.mesh.primitives[0].normals.clear(); f.upload(q); expect(f.render(q,sky),lambert(f,e,Vec3d::UnitZ()));
    Trsd mirror; mirror.scale.x()=-1; check(f.scene->set_local_transform(f.entity,mirror));
    expect(f.render(q,sky),lambert(f,e,Vec3d::UnitZ())); check(f.scene->set_local_transform(f.entity,{}));
    auto straight=e; straight.sun.direction={0,0,1}; sky=f.prepare(q,straight);
    f.asset.mesh.primitives[0].normals={{0,0,1},{0,0,1},{.6f,.8f,0},{.6f,.8f,0}}; f.upload(q);
    expect(f.render(q,sky),lambert(f,straight,Vec3d{.6,.8,1}));
    // Exactly cancelling interpolation must use the geometric fallback and stay finite.
    f.asset.mesh.primitives[0].normals={{0,0,1},{0,0,1},{0,0,-1},{0,0,-1}}; f.upload(q);
    expect(f.render(q,sky),lambert(f,straight,Vec3d::UnitZ())); sky=f.prepare(q,e);
    f.asset.mesh.primitives[0].normals.assign(4,Vec3f{.4f,.7f,1}); f.upload(q);
    Trsd parent; parent.translation=f.anchor; parent.scale={1.3,.7,1.4}; check(f.scene->set_local_transform(f.parent,parent));
    Trsd child; child.rotation=Eigen::AngleAxisd(.3,Vec3d{1,1,0}.normalized()); child.scale={-.8,1.1,.9}; check(f.scene->set_local_transform(f.entity,child));
    auto view=f.view(); const auto entities=view.scene().entities();
    auto entity=std::ranges::find(entities,f.entity,&RenderEntity::id);
    const Vec3d expected_normal=take(entity->world.inverse()).matrix().topLeftCorner<3,3>().transpose()*Vec3d{.4,.7,1};
    expect(f.render(q,sky),lambert(f,e,expected_normal));
    f.place(); check(f.scene->set_local_transform(f.entity,{}));
    f.asset.mesh.primitives[0].normals.assign(4,Vec3f{0,0,1});
    f.asset.mesh.primitives[0].indices={0,2,1,0,3,2}; f.upload(q);
    expect(f.render(q,sky),f.settings.clear_rgb.cast<double>());
    f.asset.materials[0].double_sided=true; f.upload(q);
    auto back=e; back.sun.direction={0,0,-1}; sky=f.prepare(q,back);
    expect(f.render(q,sky),lambert(f,back,-Vec3d::UnitZ()));
    f.asset.mesh.primitives[0].indices={0,1,2,0,2,3}; f.asset.materials[0].double_sided=false;
    TextureData texture; texture.id=take(AssetId::generate()); texture.width=texture.height=1;
    texture.rgba8={std::byte{128},std::byte{128},std::byte{128},std::byte{0}}; f.asset.textures.push_back(texture);
    f.asset.materials[0].base_color_texture=texture.id; f.asset.materials[0].alpha_mode=AlphaMode::mask; f.upload(q);
    sky=f.prepare(q,e); expect(f.render(q,sky),f.settings.clear_rgb.cast<double>());
    f.asset.textures[0].rgba8[3]=std::byte{255}; f.upload(q);
    const Vec3d emission=f.asset.materials[0].emissive.cast<double>();
    expect(f.render(q,sky),(lambert(f,e,Vec3d::UnitZ())-emission)*std::pow((128./255+.055)/1.055,2.4)+emission);
    std::puts("normal inverse transpose, shear, reflection, sidedness, absent/zero/large normals, mask and sRGB/emissive passed");
}
void direct(Fixture& f,SubmissionQueue& q) {
    for(double h : {1.,1000.,100000.,150000.}) {
        f.anchor.y()=h; f.place();
        for(double elevation : {.8,.04,-.1,-.2}) {
            EnvironmentDescription e; e.enabled=true; e.sun.direction={0,elevation,std::sqrt(1-elevation*elevation)};
            e.sun.irradiance={2,1,.5}; auto sky=f.prepare(q,e); const auto frame=f.render(q,sky);
            const auto t=take(atmosphere_transmittance_reference(e.atmosphere,h,elevation,2048));
            expect(frame,lambert(f,e,Vec3d::UnitZ(),t));
            std::printf("direct h=%.0f sun_y=%.2f T=(%.6f %.6f %.6f)\n",h,elevation,t.x(),t.y(),t.z());
            if(h==1 && elevation==.04) save(frame,"sunlight-sunset.ppm");
            f.settings.exposure=3; expect(f.render(q,sky),lambert(f,e,Vec3d::UnitZ(),t),3); f.settings.exposure=1;
        }
    }
    // A low sky anchor must not force high receivers to use its direct transmittance.
    f.anchor={0,100000,0}; f.place();
    EnvironmentDescription high; high.enabled=true; high.sun.direction={0,.4,std::sqrt(.84)};
    check(f.scene->set_environment(high));
    auto low_sky=take(f.atmosphere.prepare(q,f.scene->environment(),Vec3d{0,1,0}));
    expect(f.render(q,low_sky),lambert(f,high,Vec3d::UnitZ(),take(atmosphere_transmittance_reference(high.atmosphere,100000,.4,2048))));
    // Translate the entire planet far from the world origin, then move along its curved surface.
    f.anchor={101000000,200000100,300000000}; f.place();
    EnvironmentDescription translated; translated.enabled=true;
    translated.atmosphere.planet_center_m={100000000,200000000-translated.atmosphere.ground_radius_m,300000000};
    translated.sun.direction={1,.2,.5}; translated=take(normalize_environment(translated));
    auto translated_sky=f.prepare(q,translated);
    const Vec3d radial=f.anchor-Vec3d{translated.atmosphere.planet_center_m[0],translated.atmosphere.planet_center_m[1],translated.atmosphere.planet_center_m[2]};
    const Vec3d sun{translated.sun.direction[0],translated.sun.direction[1],translated.sun.direction[2]};
    const auto t=take(atmosphere_transmittance_reference(translated.atmosphere,radial.norm()-translated.atmosphere.ground_radius_m,radial.normalized().dot(sun),2048));
    expect(f.render(q,translated_sky),lambert(f,translated,Vec3d::UnitZ(),t));
    f.anchor={0,100,0}; f.place(); auto e=vacuum(); auto sky=f.prepare(q,e);
    auto unlit=take(f.pipeline.render(q,f.view(),f.assets,f.settings)); require(take(unlit.wait(q)),"unlit wait"); const auto baseline=pixels(unlit);
    e.enabled=false; sky=f.prepare(q,e); require(pixels(f.render(q,sky))==baseline,"disabled changed unlit image");
    std::puts("atmospheric attenuation, planet shadow, outside-top receivers, exposure and disabled/unlit passed");
}
Vec3d sample(const AtmosphereLutView& lut,double u,double v) {
    u=std::clamp(u,0.,double(lut.width-1)); v=std::clamp(v,0.,double(lut.height-1));
    const auto x=unsigned(u),y=unsigned(v),x1=std::min(x+1,lut.width-1),y1=std::min(y+1,lut.height-1);
    auto rgb=[&](unsigned px,unsigned py){const auto p=lut.texels[std::size_t(py)*lut.width+px];return Vec3d{p[0],p[1],p[2]};};
    return ((1-(u-x))*rgb(x,y)+(u-x)*rgb(x1,y))*(1-(v-y))+((1-(u-x))*rgb(x,y1)+(u-x)*rgb(x1,y1))*(v-y);
}
Vec3d sky_irradiance(const AtmosphereSky& sky,const Vec3d& world_normal,unsigned side) {
    const auto e=take(sky.environment()); const auto p=take(sky.position_m());
    const Vec3d center{e.atmosphere.planet_center_m[0],e.atmosphere.planet_center_m[1],e.atmosphere.planet_center_m[2]};
    const Vec3d up=(p-center).normalized(),sun{e.sun.direction[0],e.sun.direction[1],e.sun.direction[2]};
    Vec3d x=sun-up*up.dot(sun); if(x.norm()<1e-8) x=up.unitOrthogonal(); else x.normalize();
    const Vec3d z=x.cross(up),n{world_normal.dot(x),world_normal.dot(up),world_normal.dot(z)};
    const double h=take(atmosphere_height_m(e.atmosphere,{p.x(),p.y(),p.z()})),r=e.atmosphere.ground_radius_m;
    const double eh=std::asin(-std::sqrt(h*(2*r+h))/(r+h)),pi=std::numbers::pi;
    const auto lut=take(sky.lut(AtmosphereLut::sky_view)); Vec3d sum=Vec3d::Zero();
    for(unsigned y=0;y<side;++y) for(unsigned col=0;col<side*2;++col) {
        const double mu=1-2*(y+.5)/side,phi=2*pi*(col+.5)/(side*2)-pi,s=std::sqrt(1-mu*mu),el=std::asin(mu);
        const Vec3d d{s*std::cos(phi),mu,s*std::sin(phi)};
        const double row=el<eh ? 53*(1-std::sqrt((eh-el)/(eh+pi/2))) : 54+53*std::sqrt((el-eh)/(pi/2-eh));
        sum+=sample(lut,(phi/pi*.5+.5)*191,row)*std::max(0.,n.dot(d));
    }
    return sum*(4*pi/(side*side*2));
}
void diffuse(Fixture& f,SubmissionQueue& q) {
    for(double elevation : {.8,.04,-.05}) {
        EnvironmentDescription e; e.enabled=true; e.sun.direction={0,elevation,std::sqrt(1-elevation*elevation)};
        e.sun.irradiance={2,1,.5}; auto sky=f.prepare(q,e); f.settings.sky_diffuse=true;
        for(Vec3d n : {Vec3d{0,1,0},Vec3d{0,0,1},Vec3d{1,1,.2}.normalized().eval(),Vec3d{0,-1,0}}) {
            f.asset.mesh.primitives[0].normals.assign(4,n.cast<float>()); f.upload(q);
            const auto a=sky_irradiance(sky,n,64),b=sky_irradiance(sky,n,128);
            require((a-b).norm()/std::max(b.norm(),.001)<.01,"sky CPU angular reference did not converge");
            const auto t=take(atmosphere_transmittance_reference(e.atmosphere,100,elevation,2048));
            const Vec3d energy{2,1,.5};
            const Vec3d expected=lambert(f,e,n,t)+f.asset.materials[0].base_color.head<3>().cast<double>().cwiseProduct(energy).cwiseProduct(b)/std::numbers::pi;
            expect(f.render(q,sky),expected);
        }
        save(f.render(q,sky),"sunlight-sky-"+std::to_string(elevation)+".ppm");
    }
    std::puts("sky diffuse vs converged 32768-direction angular reference passed");
}
void failures(Fixture& f,SubmissionQueue& q,const DeviceOptions& options,memory::MemorySystem& system) {
    auto e=vacuum(); auto sky=f.prepare(q,e); auto view=f.view(); auto first=f.render(q,sky); const auto baseline=pixels(first); const auto generations=sky.generations();
    const auto submitted=q.stats().submitted; const auto base=allocations(q);
    auto unchanged=[&]{require(q.stats().submitted==submitted && allocations(q)==base && pixels(first)==baseline && f.atmosphere.generations()==generations,"failure changed old frame, cache or allocations");};
    auto mismatch=e; mismatch.sun.irradiance[0]*=2; check(f.scene->set_environment(mismatch));
    require(!f.pipeline.render_sunlight(q,f.view(),f.assets,sky),"mismatched environment accepted"); unchanged(); check(f.scene->set_environment(e));
    require(!f.pipeline.render_sunlight(q,view,f.assets,AtmosphereSky{}),"empty sky accepted"); unchanged();
    Trsd singular; singular.scale.z()=0; check(f.scene->set_local_transform(f.entity,singular));
    require(!f.pipeline.render_sunlight(q,f.view(),f.assets,sky),"singular normal transform accepted"); unchanged(); check(f.scene->set_local_transform(f.entity,{}));
    Trsd underground; underground.translation.y()=-101; check(f.scene->set_local_transform(f.entity,underground));
    require(!f.pipeline.render_sunlight(q,f.view(),f.assets,sky),"underground receiver accepted"); unchanged(); check(f.scene->set_local_transform(f.entity,{}));
    graphics::detail::ObjectAccess::fail_creation(q.resources(),VK_ERROR_OUT_OF_HOST_MEMORY);
    require(!f.pipeline.render_sunlight(q,view,f.assets,sky),"object failure ignored"); unchanged();
    fail_submit=true; require(!f.pipeline.render_sunlight(q,view,f.assets,sky),"submit failure ignored"); unchanged();
    auto other=take(SubmissionQueue::create(f.heap,take(Device::create(f.heap,options))));
    require(!f.pipeline.render_sunlight(other,view,f.assets,sky),"foreign queue accepted"); unchanged();
    {
        auto foreign_renderer=take(AtmosphereRenderer::create(f.heap,other,DK_ATMOSPHERE_SHADER_DIR));
        auto foreign_sky=take(foreign_renderer.prepare(other,e,f.anchor));
        require(!f.pipeline.render_sunlight(q,view,f.assets,foreign_sky),"foreign LUT accepted"); unchanged();
    }
    check(other.close());
    auto limited=take(system.create_heap({"sunlight-budget",memory::DomainCategory::render,524288}));
    {
        auto pipeline=take(ScenePipeline::create(limited,q,DK_RENDER_SHADER_DIR));
        auto warm=take(pipeline.render_sunlight(q,view,f.assets,sky)); require(take(warm.wait(q)),"budget warmup"); warm={};
        const auto count=allocations(q);
        struct Block{void* p;std::size_t size;}; std::vector<Block> blocks;
        while(auto b=limited.try_allocate(2048)) blocks.push_back({*b,2048});
        while(auto b=limited.try_allocate(1)) blocks.push_back({*b,1});
        bool failed=false,partial=false,success=false;
        do {
            const auto before=limited.snapshot(); const auto submissions=q.stats().submitted;
            {
                auto frame=pipeline.render_sunlight(q,view,f.assets,sky,f.settings);
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
        auto closing=take(pipeline.render_sunlight(q,view,f.assets,sky,f.settings)); limited.begin_close();
        require(!pipeline.render_sunlight(q,view,f.assets,sky),"closing heap accepted new frame");
        require(take(closing.wait(q)),"closing heap broke old frame"); require(pixels(closing)==baseline,"closing frame changed");
    }
    require(limited.snapshot().live_allocations==0,"lighting metadata leaked after closing");
    auto pending=take(f.pipeline.render_sunlight(q,view,f.assets,sky,f.settings));
    auto untouched=std::vector<std::byte>(33*33*4,std::byte{42}); const auto sentinel=untouched;
    require(!pending.read_rgba8(untouched) && untouched==sentinel,"pending read changed output");
    auto newer=f.prepare(q,mismatch); require(pixels(first)==baseline,"new sky changed old image");
    require(take(pending.wait(q)),"cache-hit prepare does not wait for pending frame");
    check(pending.read_rgba8(untouched)); require(untouched==baseline,"in-flight frame used new environment");
    auto last=take(f.pipeline.render_sunlight(q,f.view(),f.assets,newer));
    f.assets.clear(); f.pipeline={}; f.atmosphere={}; sky={}; newer={};
    require(take(last.wait(q)),"owner destruction broke pending frame");
    expect(last,lambert(f,take(normalize_environment(mismatch)),Vec3d::UnitZ()));
    std::puts("snapshot matching, empty/singular/domain/object/submit/foreign/budget failures, closing, pending read and old owners passed");
}
}
int main(int argc,char** argv) {
    Diagnostics d;
    try {
        require(argc==2,"expected normals/direct/diffuse/failures"); const std::string mode=argv[1];
        auto system=take(memory::MemorySystem::create()); auto heap=take(system.create_heap({"surface-sunlight",memory::DomainCategory::render}));
        {
            memory::ThreadContext context{system}; memory::ExecutionScope scope{context,heap};
            DeviceOptions options; options.validation=ValidationMode::required; options.diagnostic_sink=diagnostic; options.diagnostic_user_data=&d;
            auto device=take(Device::create(heap,options)); native_submit=reinterpret_cast<PFN_vkQueueSubmit2>(device.device_proc("vkQueueSubmit2"));
            auto q=take(graphics::detail::SubmissionAccess::create(heap,std::move(device),3,{submit}));
            std::printf("GPU=%s driver=%s\n",q.device().adapter().properties.deviceName.data(),q.device().adapter().driver.driverInfo.data());
            { Fixture f{heap,q};
                if(mode=="normals") normals(f,q); else if(mode=="direct") direct(f,q);
                else if(mode=="diffuse") diffuse(f,q); else if(mode=="failures") failures(f,q,options,system); else throw std::runtime_error("unknown mode");
            }
            check(q.close()); require(allocations(q)==0,"GPU allocation leak");
        }
        std::printf("sunlight comparisons=%u max_byte_error=%u errors=%u warnings=%u live=%zu\n",comparisons,max_byte_error,d.errors.load(),d.warnings.load(),heap.snapshot().live_allocations);
        return d.errors==0 && d.warnings==0 && heap.snapshot().live_allocations==0 && system.try_close().closed() ? 0 : 1;
    } catch(const std::exception& error) { std::fprintf(stderr,"sunlight probe failed: %s\n",error.what()); return 1; }
}

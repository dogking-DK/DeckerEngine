#include <dk/render/Atmosphere.hpp>
#include <dk/memory/MemorySystem.hpp>
#include "SubmissionInternal.hpp"
#include "ObjectInternal.hpp"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <numbers>
#include <stdexcept>
#include <vector>
using namespace dk;
using namespace dk::graphics;
using namespace dk::render;
namespace {
using Pixel=std::array<float,4>;
template<class T,class E> T take(std::expected<T,E> r) {
    if(!r) { if constexpr(std::is_same_v<E,Error>) throw std::runtime_error(r.error().message); else throw std::bad_alloc{}; }
    return std::move(*r);
}
void check(Result<void> r) { if(!r) throw std::runtime_error(r.error().message); }
void require(bool b,const char* message) { if(!b) throw std::runtime_error(message); }
struct Diagnostics { std::atomic<unsigned> errors=0,warnings=0; };
void diagnostic(void* pointer,const Diagnostic& message) noexcept {
    auto& d=*static_cast<Diagnostics*>(pointer);
    if(message.severity==VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) ++d.errors;
    if(message.severity==VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) ++d.warnings;
    if(message.severity>=VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
        std::fprintf(stderr,"%.*s: %.*s\n",int(message.name.size()),message.name.data(),int(message.message.size()),message.message.data());
}
PFN_vkQueueSubmit2 native_submit=nullptr;
unsigned fail_after=0;
VKAPI_ATTR VkResult VKAPI_CALL submit(VkQueue queue,std::uint32_t count,const VkSubmitInfo2* info,VkFence fence) {
    if(fail_after && --fail_after==0) return VK_ERROR_OUT_OF_HOST_MEMORY;
    return native_submit(queue,count,info,fence);
}
std::uint32_t allocations(const SubmissionQueue& queue) {
    VmaTotalStatistics stats{}; vmaCalculateStatistics(queue.device().allocator(),&stats); return stats.total.statistics.allocationCount;
}
std::vector<Pixel> pixels(AtmosphereRenderer& renderer,SubmissionQueue& queue,const AtmosphereSky& sky,const SkyCamera& camera) {
    auto frame=take(renderer.render_sky(queue,sky,camera)); require(take(frame.wait(queue)),"sky wait failed");
    std::vector<Pixel> result(std::size_t{camera.width}*camera.height); check(frame.read_hdr(result));
    for(const auto& pixel:result) for(auto value:pixel) require(std::isfinite(value) && value>=0,"nonfinite or negative frame");
    return result;
}
Vec3d ray(const SkyCamera& camera,unsigned x,unsigned y) {
    const Vec3d forward=camera.forward.normalized(),right=forward.cross(camera.up).normalized(),up=right.cross(forward);
    const double v=std::tan(camera.vertical_fov_rad/2);
    return (forward+right*((2*(x+.5)/camera.width-1)*v*camera.width/camera.height)-up*((2*(y+.5)/camera.height-1)*v)).normalized();
}
double metrics(const std::vector<Pixel>& actual,const std::vector<Pixel>& reference,const char* name) {
    std::vector<double> luminance,errors;
    for(const auto& p:reference) luminance.push_back(.2126*p[0]+.7152*p[1]+.0722*p[2]);
    std::sort(luminance.begin(),luminance.end()); const double scale=std::max(.001,luminance[std::size_t(.95*(luminance.size()-1))]);
    double squared=0,maximum=0;
    for(std::size_t i=0;i<actual.size();++i) for(unsigned c=0;c<3;++c) {
        const double error=std::abs(actual[i][c]-reference[i][c])/scale;
        squared+=error*error; maximum=std::max(maximum,error); errors.push_back(error);
    }
    std::sort(errors.begin(),errors.end()); const double rmse=std::sqrt(squared/errors.size()),p99=errors[std::size_t(.99*(errors.size()-1))];
    std::printf("%s scale=%.9g NRMSE=%.6g p99=%.6g max=%.6g\n",name,scale,rmse,p99,maximum); std::fflush(stdout);
    require(rmse<=.03 && p99<=.08,"sky LUT exceeds frozen numerical thresholds"); return scale;
}
void transmittance(const AtmosphereSky& sky,const AtmosphereParameters& a) {
    const auto lut=take(sky.lut(AtmosphereLut::transmittance)); double maximum=0;
    const double H=std::sqrt(a.atmosphere_height_m*(2*a.ground_radius_m+a.atmosphere_height_m));
    for(unsigned y:{0u,1u,4u,16u,32u,48u,62u,63u}) for(unsigned x:{0u,3u,31u,91u,173u,231u,254u}) {
        const double rho=H*y/(lut.height-1),h=rho*rho/(std::sqrt(a.ground_radius_m*a.ground_radius_m+rho*rho)+a.ground_radius_m);
        const double d=std::lerp(a.atmosphere_height_m-h,rho+H,double(x)/(lut.width-1));
        const double mu=d>1e-7 ? std::clamp((H*H-rho*rho-d*d)/(2*(a.ground_radius_m+h)*d),-1.,1.) : 1;
        const auto reference=take(atmosphere_transmittance_reference(a,h,mu,2048));
        for(unsigned c=0;c<3;++c) {
            const double error=std::abs(lut.texels[y*lut.width+x][c]-reference[c]);
            if(error>.002) std::printf("T error x=%u y=%u c=%u h=%.12g mu=%.12g gpu=%.9g cpu=%.9g\n",x,y,c,h,mu,lut.texels[y*lut.width+x][c],reference[c]);
            maximum=std::max(maximum,error);
        }
    }
    double filtered=0;
    for(double h:{1.,1234.,17000.,99999.}) {
        const double rho=std::sqrt(h*(2*a.ground_radius_m+h));
        const double horizon=-rho/(a.ground_radius_m+h);
        for(double mu:{1.,.13,.0,horizon+.00001}) {
            const double b=(a.ground_radius_m+h)*mu;
            const double d=-b+std::sqrt(b*b+(a.atmosphere_height_m-h)*(2*a.ground_radius_m+h+a.atmosphere_height_m));
            const double x=std::clamp((d-(a.atmosphere_height_m-h))/(rho+H-(a.atmosphere_height_m-h)),0.,1.)*(lut.width-1);
            const double y=rho/H*(lut.height-1);
            const auto x0=static_cast<unsigned>(x),y0=static_cast<unsigned>(y),x1=std::min(x0+1,lut.width-1),y1=std::min(y0+1,lut.height-1);
            const auto reference=take(atmosphere_transmittance_reference(a,h,mu,2048));
            for(unsigned c=0;c<3;++c) {
                const double value=std::lerp(std::lerp(double(lut.texels[y0*lut.width+x0][c]),double(lut.texels[y0*lut.width+x1][c]),x-x0),
                    std::lerp(double(lut.texels[y1*lut.width+x0][c]),double(lut.texels[y1*lut.width+x1][c]),x-x0),y-y0);
                filtered=std::max(filtered,std::abs(value-reference[c]));
            }
        }
    }
    std::printf("transmittance off-grid bilinear max absolute=%.9g\n",filtered);
    require(filtered<=.002,"filtered transmittance LUT error");
    std::printf("transmittance max absolute=%.9g\n",maximum); require(maximum<=.002,"transmittance LUT error");
}
void comparisons(AtmosphereRenderer& renderer,SubmissionQueue& queue) {
    EnvironmentDescription e; e.enabled=true;
    struct Case { const char* name; double height,sun_mu,elevation; };
    for(const auto c:{Case{"noon-1m",1,1,.15},Case{"sunset-1m",1,.015,.05},Case{"twilight-1m",1,-.04,.02},
        Case{"high-10km",10000,.5,-.02},Case{"top-100km",100000,.3,-.15},Case{"outside-150km",150000,.1,-.2}}) {
        e.sun.direction={std::sqrt(1-c.sun_mu*c.sun_mu),c.sun_mu,0};
        const Vec3d position{0,c.height,0}; SkyCamera camera; camera.width=32; camera.height=18; camera.sun_disk=false;
        camera.forward={std::cos(c.elevation),std::sin(c.elevation),0}; camera.vertical_fov_rad=1.2;
        for(bool multiple:{false,true}) {
            const auto sky=take(renderer.prepare(queue,e,position,multiple));
            if(c.height==1 && c.sun_mu==1 && !multiple) transmittance(sky,e.atmosphere);
            const auto lut=pixels(renderer,queue,sky,camera); camera.reference=true;
            const auto reference=pixels(renderer,queue,sky,camera); camera.reference=false;
            const double scale=metrics(lut,reference,(std::string{c.name}+(multiple ? " MS" : " single")).c_str());
            if(!multiple) {
                double maximum=0;
                for(const auto xy:{std::array{0u,0u},std::array{15u,8u},std::array{31u,17u},std::array{20u,4u}}) {
                    const auto cpu=take(integrate_atmosphere_reference(e,position,ray(camera,xy[0],xy[1]),{1024,1024}));
                    for(unsigned channel=0;channel<3;++channel) maximum=std::max(maximum,std::abs(reference[xy[1]*camera.width+xy[0]][channel]-cpu.radiance[channel])/scale);
                }
                std::printf("%s GPU direct / CPU double max normalized=%.9g\n",c.name,maximum); std::fflush(stdout);
                require(maximum<=.008,"GPU direct integration differs from CPU double");
            }
        }
    }
    e.sun.direction={1,.3,0}; const auto sky=take(renderer.prepare(queue,e,Vec3d{0,1,0},false));
    SkyCamera camera; camera.width=camera.height=1; camera.sun_disk=false;
    double maxBoundary=0;
    const double horizon=-std::sqrt(1*(2*e.atmosphere.ground_radius_m+1))/(e.atmosphere.ground_radius_m+1);
    for(double mu:{horizon-.0005,horizon+.0005,0.,1.,-.1}) {
        camera.forward={std::sqrt(1-mu*mu),mu,0}; camera.up=std::abs(mu)>.99 ? Vec3d{1,0,0} : Vec3d{0,1,0};
        const auto actual=pixels(renderer,queue,sky,camera)[0];
        const auto cpu=take(integrate_atmosphere_reference(e,Vec3d{0,1,0},camera.forward,{2048,2048}));
        const double scale=std::max(.001,cpu.radiance.maxCoeff());
        for(unsigned c=0;c<3;++c) maxBoundary=std::max(maxBoundary,std::abs(actual[c]-cpu.radiance[c])/scale);
    }
    std::printf("horizon/zenith explicit rays max normalized=%.9g\n",maxBoundary);
    require(maxBoundary<=.08,"horizon edge exceeded explicit point threshold");
    for(double h:{10000.,100000.,150000.}) {
        const auto high=take(renderer.prepare(queue,e,Vec3d{0,h,0},false));
        const double edge=-std::sqrt(h*(2*e.atmosphere.ground_radius_m+h))/(e.atmosphere.ground_radius_m+h);
        double error=0;
        for(double mu:{edge-.0005,edge+.0005}) for(double azimuth:{-1.,1.}) {
            camera.forward={azimuth*std::sqrt(1-mu*mu),mu,0}; camera.up={0,1,0};
            const auto fast=pixels(renderer,queue,high,camera)[0]; camera.reference=true;
            const auto exact=pixels(renderer,queue,high,camera)[0]; camera.reference=false;
            const double scale=std::max(.001,double(std::max({exact[0],exact[1],exact[2]})));
            for(unsigned c=0;c<3;++c) error=std::max(error,std::abs(fast[c]-exact[c])/scale);
        }
        std::printf("high horizon %.0fm max normalized=%.9g\n",h,error); std::fflush(stdout);
        require(error<=.08,"high horizon edge exceeded explicit point threshold");
    }
}
void multiple_comparison(AtmosphereRenderer& renderer,SubmissionQueue& queue) {
    EnvironmentDescription e; e.enabled=true;
    const auto sky=take(renderer.prepare(queue,e,Vec3d{0,1,0}));
    const auto table=take(sky.lut(AtmosphereLut::multiple_scattering));
    for(const auto xy:{std::array{23u,3u},std::array{30u,12u},std::array{15u,24u}}) {
        const double h=e.atmosphere.atmosphere_height_m*std::pow(double(xy[1])/31,2),mu=2*double(xy[0])/31-1;
        const auto cpu=take(multiple_scattering_reference(e.atmosphere,h,mu,64,128,256));
        double relative=0;
        for(unsigned c=0;c<3;++c) relative=std::max(relative,std::abs(table.texels[xy[1]*32+xy[0]][c]-cpu.radiance[c])/std::max(1e-5,cpu.radiance[c]));
        std::printf("MS closure x=%u y=%u vs 4096 directions max relative=%.9g\n",xy[0],xy[1],relative); std::fflush(stdout);
        std::printf("GPU RGB %.9g %.9g %.9g CPU RGB %.9g %.9g %.9g\n",table.texels[xy[1]*32+xy[0]][0],table.texels[xy[1]*32+xy[0]][1],table.texels[xy[1]*32+xy[0]][2],cpu.radiance[0],cpu.radiance[1],cpu.radiance[2]);
        require(relative<.15,"MS quadrature too far from independent high-sample closure");
    }
}
void continuity(AtmosphereRenderer& renderer,SubmissionQueue& queue) {
    EnvironmentDescription e; e.enabled=true; e.sun.direction={1,.015,0};
    SkyCamera camera; camera.width=camera.height=1; camera.sun_disk=false;
    for(double h:{1.,10000.,100000.,150000.}) for(bool multiple:{false,true}) {
        const auto sky=take(renderer.prepare(queue,e,Vec3d{0,h,0},multiple));
        const double edge=-std::sqrt(h*(2*e.atmosphere.ground_radius_m+h))/(e.atmosphere.ground_radius_m+h);
        std::vector<Pixel> fast,exact;
        double scale=.001,point=0,step=0;
        for(double offset:{-.0005,-.00025,-.000125,-.0000625,.0000625,.000125,.00025,.0005}) {
            const double mu=edge+offset; camera.forward={std::sqrt(1-mu*mu),mu,0};
            camera.reference=false; fast.push_back(pixels(renderer,queue,sky,camera)[0]);
            camera.reference=true; exact.push_back(pixels(renderer,queue,sky,camera)[0]);
            for(unsigned c=0;c<3;++c) scale=std::max(scale,double(exact.back()[c]));
        }
        for(std::size_t i=0;i<fast.size();++i) for(unsigned c=0;c<3;++c) {
            point=std::max(point,std::abs(double(fast[i][c])-exact[i][c])/scale);
            if(i) step=std::max(step,std::abs((double(fast[i][c])-fast[i-1][c])-(double(exact[i][c])-exact[i-1][c]))/scale);
        }
        std::printf("horizon continuity h=%.0f MS=%d scale=%.9g point=%.9g step residual=%.9g\n",h,multiple,scale,point,step);
        require(point<=.08 && step<=.08,"horizon continuity differs from direct reference");
    }
}
void cache_and_failures(memory::MemorySystem& system,memory::ResourceHandle heap,SubmissionQueue& queue,const DeviceOptions& options) {
    check(queue.configure_gpu_profiling({true}));
    auto budget=take(system.create_heap({"atmosphere-budget",memory::DomainCategory::render,8*1024*1024}));
    auto renderer=take(AtmosphereRenderer::create(budget,queue,DK_ATMOSPHERE_SHADER_DIR));
    EnvironmentDescription e; e.enabled=true; Vec3d position{0,1,0};
    auto sky=take(renderer.prepare(queue,e,position)); auto generations=sky.generations();
    SkyCamera camera; camera.width=16; camera.height=8; camera.sun_disk=false;
    const auto baseline=pixels(renderer,queue,sky,camera);
    auto prepare=[&](unsigned t,unsigned m,unsigned s) {
        const auto old=generations; sky=take(renderer.prepare(queue,e,position)); generations=sky.generations();
        require(generations==AtmosphereGenerations{old.transmittance+t,old.multiple_scattering+m,old.sky_view+s},"cache rebuilt incorrect LUTs");
    };
    const auto original=sky;
    const auto profile=take(original.lut_profile(AtmosphereLut::sky_view));
    require(profile.status()==GpuProfileStatus::ready && !profile.timings().empty(),"LUT profile missing");
    require(!AtmosphereSky{}.lut_profile(AtmosphereLut::sky_view) && !sky.lut_profile(AtmosphereLut{99}),"invalid LUT profile accepted");
    prepare(0,0,0);
    require(take(sky.lut_profile(AtmosphereLut::sky_view)).submission_value()==profile.submission_value(),"cache hit replaced observation");
    e.sun.irradiance={2,3,4}; e.sun.angular_radius_rad*=1.5; prepare(0,0,0);
    const auto energy=pixels(renderer,queue,sky,camera);
    for(std::size_t i=0;i<energy.size();++i) for(unsigned c=0;c<3;++c)
        require(std::abs(energy[i][c]-baseline[i][c]*e.sun.irradiance[c])<1e-5,"sun energy applied incorrectly");
    camera.width=9; camera.height=7; camera.forward={-1,.2,0}; static_cast<void>(pixels(renderer,queue,sky,camera));
    require(renderer.generations()==generations,"camera orientation/extent dirtied LUTs");
    e.sun.direction={1,1,0}; prepare(0,0,1);
    e.sun.direction={0,1,1}; prepare(0,0,0); // Same local zenith, different azimuth.
    position.y()=1000; prepare(0,0,1);
    e.atmosphere.mie_anisotropy=.6; prepare(0,0,1);
    e.atmosphere.ground_albedo={.2,.4,.1}; prepare(0,1,1);
    e.atmosphere.mie_scattering_per_m[0]*=.8; prepare(0,1,1);
    e.atmosphere.rayleigh_scattering_per_m[0]*=1.1; prepare(1,1,1);
    e.atmosphere.planet_center_m[0]+=50; position.x()+=50; prepare(0,0,0);
    auto bad=e; bad.atmosphere.ground_radius_m=10;
    require(!renderer.prepare(queue,bad,position) && renderer.generations()==generations,"invalid domain changed cache");
    auto next=e; next.atmosphere.rayleigh_scattering_per_m[1]*=.9;
    const auto base=allocations(queue);
    auto opaque=e; opaque.atmosphere.rayleigh_scattering_per_m={1,1,1};
    opaque.atmosphere.mie_scattering_per_m={}; opaque.atmosphere.mie_extinction_per_m={}; opaque.atmosphere.absorption_extinction_per_m={};
    require(!renderer.prepare(queue,opaque,position) && renderer.generations()==generations && allocations(queue)==base,"nonconvergent candidate published or leaked");
    graphics::detail::ObjectAccess::fail_creation(queue.resources(),VK_ERROR_OUT_OF_HOST_MEMORY);
    require(!renderer.prepare(queue,next,position),"object creation failure ignored");
    require(renderer.generations()==generations && allocations(queue)==base,"creation failure changed cache or leaked");
    for(unsigned failure:{1u,2u,3u}) {
        fail_after=failure; require(!renderer.prepare(queue,next,position),"submit failure ignored");
        require(renderer.generations()==generations && allocations(queue)==base,"partial prepare failure published or leaked");
    }
    require(take(original.lut_profile(AtmosphereLut::sky_view)).submission_value()==profile.submission_value(),"failed rebuild changed old observation");
    std::vector<void*> blocks;
    while(auto block=budget.try_allocate(4096)) blocks.push_back(*block);
    require(!renderer.prepare(queue,next,position) && renderer.generations()==generations,"budget failure changed cache");
    for(auto block:blocks) budget.deallocate(block,4096);
    require(allocations(queue)==base,"budget failure leaked GPU resources");
    auto other=take(SubmissionQueue::create(heap,take(Device::create(heap,options)),3));
    require(!renderer.prepare(other,e,position) && !renderer.render_sky(other,sky,camera),"foreign queue accepted"); check(other.close());
    camera={}; camera.width=16; camera.height=8; camera.sun_disk=false;
    require(pixels(renderer,queue,original,camera)==baseline,"old snapshot pixels changed");
    auto oldFrame=take(renderer.render_sky(queue,original,camera));
    std::vector<Pixel> oldPixels(baseline.size(),Pixel{42,42,42,42}); const auto untouched=oldPixels;
    require(!oldFrame.read_hdr(oldPixels) && oldPixels==untouched,"pending readback changed destination");
    static_cast<void>(take(renderer.prepare(queue,next,position)));
    check(oldFrame.read_hdr(oldPixels)); require(oldPixels==baseline,"old frame changed during LUT update"); oldFrame={};
    camera.width=0; require(!renderer.render_sky(queue,sky,camera),"zero extent accepted");
    camera.width=16; camera.up=camera.forward; require(!renderer.render_sky(queue,sky,camera),"parallel camera basis accepted");
    camera.up={0,1,0}; auto pending=take(renderer.render_sky(queue,sky,camera));
    static_cast<void>(budget.try_close());
    require(!renderer.prepare(queue,e,position) && !renderer.render_sky(queue,sky,camera),"closing renderer memory accepted work");
    pending={}; sky={}; renderer={}; require(take(queue.wait(take(queue.submit(take(queue.begin()))))),"pending owners did not drain");
    std::printf("cache granularity, immutable old snapshots, object/submit(1..3)/budget failures and foreign queue passed\n");
}
void vacuum_and_disk(AtmosphereRenderer& renderer,SubmissionQueue& queue) {
    EnvironmentDescription e; e.enabled=true;
    e.atmosphere.rayleigh_scattering_per_m={}; e.atmosphere.mie_scattering_per_m={}; e.atmosphere.mie_extinction_per_m={};
    e.atmosphere.absorption_extinction_per_m={}; e.atmosphere.ground_albedo={};
    SkyCamera camera; camera.width=camera.height=1; camera.forward={0,1,0}; camera.up={0,0,1}; camera.sun_disk=false;
    auto sky=take(renderer.prepare(queue,e,Vec3d{0,1,0}));
    for(const auto& pixel:take(sky.lut(AtmosphereLut::transmittance)).texels) require(pixel==Pixel{1,1,1,1},"vacuum transmittance not one");
    require(pixels(renderer,queue,sky,camera)[0]==Pixel{0,0,0,1},"vacuum sky nonzero");
    camera.sun_disk=true; const auto disk=pixels(renderer,queue,sky,camera)[0]; const auto expected=take(solar_disk_radiance(e.sun));
    for(unsigned c=0;c<3;++c) require(std::abs(disk[c]/expected[c]-1)<.0001,"disk energy mismatch");
    camera.forward={1,0,0}; require(pixels(renderer,queue,sky,camera)[0]==Pixel{0,0,0,1},"outside solar disk not black");
    e.sun.angular_radius_rad=1e-6; e.sun.direction={.0001,1,0}; camera.forward={.0001,1,0};
    sky=take(renderer.prepare(queue,e,Vec3d{0,1,0})); const auto narrow=pixels(renderer,queue,sky,camera)[0];
    const auto narrowEnergy=take(solar_disk_radiance(e.sun));
    std::printf("narrow disk GPU=%.12g expected=%.12g relative=%.12g\n",narrow[0],narrowEnergy[0],narrow[0]/narrowEnergy[0]-1);
    require(std::abs(narrow[0]/narrowEnergy[0]-1)<.0001,"narrow near-zenith solar disk lost");
    camera.forward.x()+=8e-6;
    require(pixels(renderer,queue,sky,camera)[0]==Pixel{0,0,0,1},"cosine rounding inflated a narrow disk");
    e.sun.direction={0,-1,0}; sky=take(renderer.prepare(queue,e,Vec3d{0,1,0})); camera.forward={0,-1,0};
    require(pixels(renderer,queue,sky,camera)[0]==Pixel{0,0,0,1},"planet did not occult solar disk");
    e.enabled=false; e.sun.direction={0,1,0}; sky=take(renderer.prepare(queue,e,Vec3d{0,1,0})); camera.forward={0,1,0};
    require(pixels(renderer,queue,sky,camera)[0]==Pixel{0,0,0,1},"disabled environment emits light");
}
}
int main(int argc,char** argv) {
    const std::string_view mode=argc>1 ? argv[1] : "reference";
    if(argc>2 || (mode!="reference" && mode!="multiple" && mode!="cache" && mode!="vacuum" && mode!="continuity")) return 2;
    Diagnostics diagnostics;
    try {
        auto system=take(memory::MemorySystem::create()); auto heap=take(system.create_heap({"atmosphere",memory::DomainCategory::render}));
        {
            DeviceOptions options; options.validation=ValidationMode::required; options.diagnostic_sink=diagnostic; options.diagnostic_user_data=&diagnostics;
            auto device=take(Device::create(heap,options)); native_submit=reinterpret_cast<PFN_vkQueueSubmit2>(device.device_proc("vkQueueSubmit2"));
            auto queue=take(graphics::detail::SubmissionAccess::create(heap,std::move(device),3,{submit}));
            std::printf("GPU=%s driver=%s\n",queue.device().adapter().properties.deviceName.data(),queue.device().adapter().driver.driverInfo.data()); std::fflush(stdout);
            {
                auto renderer=take(AtmosphereRenderer::create(heap,queue,DK_ATMOSPHERE_SHADER_DIR));
                if(mode=="reference") comparisons(renderer,queue);
                if(mode=="vacuum") vacuum_and_disk(renderer,queue);
                if(mode=="multiple") multiple_comparison(renderer,queue);
                if(mode=="continuity") continuity(renderer,queue);
            }
            if(mode=="cache") cache_and_failures(system,heap,queue,options);
            check(queue.close()); require(allocations(queue)==0,"atmosphere GPU resources leaked");
        }
        std::printf("atmosphere validation errors=%u warnings=%u live=%zu\n",diagnostics.errors.load(),diagnostics.warnings.load(),heap.snapshot().live_allocations);
        return !diagnostics.errors && !diagnostics.warnings && !heap.snapshot().live_allocations && system.try_close().closed() ? 0 : 1;
    } catch(const std::exception& error) { std::fprintf(stderr,"atmosphere probe failed: %s\n",error.what()); return 1; }
}

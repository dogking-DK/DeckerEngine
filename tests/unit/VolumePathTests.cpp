#include "../RenderAssetSupport.hpp"
#include "../PathBsdfOracle.hpp"
#include "../../engine/render/pathtracing/src/VolumePhase.hpp"
#include <dk/render/VolumePathReference.hpp>
#include <dk/scene/SceneDocument.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <array>
#include <chrono>
#include <iostream>
#include <thread>

using namespace dk;
using namespace dk::render;
namespace {
constexpr double pi=std::numbers::pi;
struct Fixture : RenderMemory {
    std::unique_ptr<SceneDocument> document=SceneDocument::create().value();
    CpuAsset asset;
    Fixture() {
        asset.mesh.id=AssetId::generate().value(); const auto entity=document->create_entity().value();
        document->set_asset_references(entity,{{asset.mesh.id,AssetKind::mesh}}).value();
    }
    void triangle(Vec3f a,Vec3f b,Vec3f c,Vec3f color,Vec3f emission={0,0,0},bool two_sided=false) {
        MaterialData m; m.id=AssetId::generate().value(); m.metallic=0;
        m.base_color={color.x(),color.y(),color.z(),1}; m.emissive=emission; m.double_sided=two_sided;
        asset.materials.push_back(m); MeshPrimitive p; p.material=m.id; p.positions={a,b,c}; p.indices={0,1,2};
        asset.mesh.primitives.push_back(std::move(p));
    }
    void quad(float extent,float z,Vec3f color,Vec3f emission={0,0,0},bool flip=false) {
        const Vec3f a{-extent,-extent,z},b{extent,-extent,z},c{extent,extent,z},d{-extent,extent,z};
        if(flip) { triangle(a,c,b,color,emission); triangle(a,d,c,color,emission); }
        else { triangle(a,b,c,color,emission); triangle(a,c,d,color,emission); }
    }
    RenderScene source() { return RenderScene::extract(heap,document->snapshot().value()).value(); }
    PathTraceScene scene(PathTraceMaterialModel model=PathTraceMaterialModel::lambert) {
        const std::array inputs{static_cast<const CpuAsset*>(&asset)};
        return PathTraceScene::create(heap,source(),inputs,{},model).value();
    }
    RenderView view(const RenderScene& source,unsigned size=8) {
        ViewDescription d{size,size}; Trsd camera; camera.translation={0,0,1}; d.camera_world=Transformd::from_trs(camera).value();
        d.projection.setZero(); d.projection(0,0)=2; d.projection(1,1)=-2;
        d.projection(2,2)=10./(.1-10); d.projection(2,3)=1./(.1-10); d.projection(3,2)=-1;
        return RenderView::create(source,d).value();
    }
};
const PathTraceRay down{{0,0,1},{0,0,-1}},up{{0,1,0},{0,1,0}};
VolumePathSettings settings(unsigned depth=1) { VolumePathSettings s; s.max_bounces=depth; s.rr_start=0; s.sun=false; return s; }
PathMediumSnapshot medium(const EnvironmentDescription& e={},const PathMediumSettings& s={},std::shared_ptr<const CloudFieldData> fields={}) {
    auto result=PathMediumSnapshot::create(e,s,std::move(fields)); REQUIRE(result); return *result;
}
EnvironmentDescription air(double extinction,double scattering) {
    EnvironmentDescription e; e.enabled=true; auto& a=e.atmosphere;
    a.atmosphere_height_m=10000; a.mie_scale_height_m=10000;
    a.absorption_center_height_m=5000; a.absorption_half_width_m=2500;
    a.rayleigh_scattering_per_m=a.absorption_extinction_per_m={0,0,0};
    a.mie_extinction_per_m={extinction,extinction,extinction}; a.mie_scattering_per_m={scattering,scattering,scattering};
    a.mie_anisotropy=0; a.ground_albedo={0,0,0}; return e;
}
struct Population {
    Vec3d mean=Vec3d::Zero(),standard_error=Vec3d::Zero();
    double scalar=0,scalar_error=0;
    VolumePathStatistics stats;
};
Population population(const PathTraceScene& scene,const PathMediumSnapshot& m,const PathTraceRay& ray,VolumePathSettings s,unsigned count=16384,unsigned pixels=1) {
    std::array<Vec3d,8> values; Population p;
    for(unsigned i=0;i<values.size();++i) {
        s.seed=101+7919*i; values[i].setZero();
        for(unsigned pixel=0;pixel<pixels;++pixel) {
            auto result=estimate_volume_path_reference(scene,m,ray,s,{0,count},pixel);
            INFO((result ? "volume estimate succeeded" : result.error().message));
            REQUIRE(result); values[i]+=result->mean/pixels;
            p.stats.medium_scatter+=result->statistics.medium_scatter; p.stats.absorption+=result->statistics.absorption;
            p.stats.multiple_scatter_paths+=result->statistics.multiple_scatter_paths;
            p.stats.null_collisions+=result->statistics.null_collisions; p.stats.tracking_steps+=result->statistics.tracking_steps;
        }
        p.mean+=values[i]/values.size();
    }
    p.scalar=p.mean.mean();
    for(const auto& v:values) {
        p.standard_error+=(v-p.mean).cwiseAbs2()/(values.size()*(values.size()-1));
        p.scalar_error+=std::pow(v.mean()-p.scalar,2)/(values.size()*(values.size()-1));
    }
    p.standard_error=p.standard_error.cwiseSqrt(); p.scalar_error=std::sqrt(p.scalar_error); return p;
}
void statistical_check(const Population& p,const Vec3d& expected) {
    std::cout<<"volume mean "<<p.mean.transpose()<<" expected "<<expected.transpose()<<" standard_error "<<p.standard_error.transpose()<<'\n';
    for(int c=0;c<3;++c) {
        INFO("channel="<<c<<" mean="<<p.mean[c]<<" expected="<<expected[c]<<" standard_error="<<p.standard_error[c]);
        CHECK(std::abs(p.mean[c]-expected[c])<=.02*std::abs(expected[c])+1e-4);
        CHECK(std::abs(p.mean[c]-expected[c])<=5*p.standard_error[c]+1e-4);
    }
}
// Independent single-scatter quadrature: exponential height profile, spherical sun paths.
// No production medium, phase, free-path, or transmission calls.
double single_scatter(const EnvironmentDescription& e,unsigned size) {
    const auto& a=e.atmosphere; const double R=a.ground_radius_m,H=a.atmosphere_height_m,S=a.mie_scale_height_m;
    const double sigma=a.mie_extinction_per_m[0],dy=H/size,cw=2*std::pow(std::sin(e.sun.angular_radius_rad/2),2);
    const double radiance=e.sun.irradiance[0]/(pi*std::pow(std::sin(e.sun.angular_radius_rad),2));
    double total=0;
    for(unsigned y=0;y<size;++y) {
        const double h=(y+.5)*dy,r=R+h,view_tau=sigma*S*(std::exp(-h/S)-std::exp(-H/S));
        double sun_t=0;
        for(unsigned j=0;j<8;++j) {
            const double cosine=1-cw*(j+.5)/8,b=r*cosine;
            const double end=((R+H)*(R+H)-r*r)/(std::sqrt(b*b+(H-h)*(2*R+H+h))+b);
            double tau=0;
            for(unsigned k=0;k<256;++k) {
                const double t=end*(k+.5)/256;
                const double height=(h*(2*R+h)+t*t+2*r*t*cosine)/(std::sqrt(r*r+t*t+2*r*t*cosine)+R);
                tau+=sigma*std::exp(-height/S)*end/256;
            }
            sun_t+=std::exp(-tau)/8;
        }
        total+=a.mie_scattering_per_m[0]*std::exp(-h/S)*std::exp(-view_tau)*sun_t*dy;
    }
    return total*radiance*(2*pi*cw)/(4*pi);
}
}

TEST_CASE("volume sampler has frozen stream and counter identity", "[volume]") {
    const std::array<unsigned,11> golden{2376321,13911191,16359791,12291876,3706904,905060,4069761,1774942,14198473,4237179,6418305};
    for(unsigned d=0;d<golden.size();++d)
        CHECK(volume_path_sample(1,17,29,3,VolumeSampleStream(d))==(golden[d]+.5)/16777216.);
    std::array<unsigned,3> channels{};
    for(unsigned i=0;i<65536;++i) {
        const double u=volume_path_sample(1,0,i,0,VolumeSampleStream::channel); REQUIRE(u>0); REQUIRE(u<1); ++channels[unsigned(3*u)];
    }
    for(const auto n:channels) CHECK(std::abs(n/65536.-1./3)<.006);
    const double base=volume_path_sample(1,0,0,0,VolumeSampleStream::free_path);
    CHECK(base!=volume_path_sample(1,0,0,0,VolumeSampleStream::free_path,1));
    CHECK(base!=volume_path_sample(1,0,0,1,VolumeSampleStream::free_path));
    CHECK(base!=volume_path_sample(1ull<<32,0,0,0,VolumeSampleStream::free_path));
}
TEST_CASE("volume phase normalization and sampled angular moments agree", "[volume]") {
    for(const auto& phase:std::array<dk::render::detail::VolumePhase,6>{{{{1,0,0},0,0},{{0,1,0},.99,0},{{0,1,0},-.99,0},
        {{0,1,0},1e-8,0},{{0,0,1},0,-.95},{{.2,.3,.5},.8,-.6}}}) {
        double integral=0,first=0,second=0;
        // High resolution integrates the narrow g=.99 forward peak without using its sampler.
        constexpr unsigned bins=1u<<22,count=262144;
        for(unsigned i=0;i<bins;++i) integral+=phase.eval(-1+2*(i+.5)/bins)*(4*pi/bins);
        CHECK(integral==Catch::Approx(1).margin(2e-5));
        for(unsigned i=0;i<count;++i) {
            const auto direction=phase.sample(Vec3d::UnitZ(),volume_path_sample(7,0,i,0,VolumeSampleStream::phase_component),
                volume_path_sample(7,0,i,0,VolumeSampleStream::phase_direction),volume_path_sample(7,0,i,0,VolumeSampleStream::phase_direction,1));
            first+=direction.z()/count; second+=direction.z()*direction.z()/count;
        }
        CHECK(first==Catch::Approx(phase.weights.y()*phase.mie_g+phase.weights.z()*phase.cloud_g).margin(.004));
        CHECK(second==Catch::Approx(phase.weights.x()*.4+phase.weights.y()*(1+2*phase.mie_g*phase.mie_g)/3+
            phase.weights.z()*(1+2*phase.cloud_g*phase.cloud_g)/3).margin(.004));
    }
}
TEST_CASE("volume vacuum and surface furnace retain Lambert and GGX energy", "[volume]") {
    Fixture f; const auto m=medium(); auto s=settings(); s.environment_rgb={.2,.4,.8};
    statistical_check(population(f.scene(),m,down,s),s.environment_rgb);
    f.quad(100,0,{.4f,.6f,.8f}); s.environment_rgb=Vec3d::Ones();
    statistical_check(population(f.scene(),m,down,s),Vec3f{.4f,.6f,.8f}.cast<double>());
    for(auto& material:f.asset.materials) { material.metallic=.7f; material.roughness=.4f; }
    const auto expected=path_oracle::furnace(Vec3f{.4f,.6f,.8f}.cast<double>(),double(.7f),double(.4f),Vec3d::UnitZ(),512);
    const auto scene=f.scene(PathTraceMaterialModel::metallic_roughness);
    statistical_check(population(scene,m,down,s),expected);
    s.estimator=PathTraceEstimator::bsdf_only; statistical_check(population(scene,m,down,s),expected);
}
TEST_CASE("volume area emitter MIS agrees with independent solid angle integral", "[volume]") {
    Fixture f; f.quad(100,0,{.6f,.6f,.6f}); f.quad(1,2,{0,0,0},{2,2,2},true);
    double expected=0; constexpr unsigned n=512;
    for(unsigned y=0;y<n;++y) for(unsigned x=0;x<n;++x) {
        const double px=-1+2*(x+.5)/n,py=-1+2*(y+.5)/n,r2=px*px+py*py+4;
        expected+=double(.6f)*2/pi*4/(r2*r2)*4/(double(n)*n);
    }
    const auto scene=f.scene(); const auto m=medium(); auto s=settings();
    statistical_check(population(scene,m,down,s),Vec3d::Constant(expected));
    auto clipped=down; clipped.maximum=1.1;
    CHECK(estimate_volume_path_reference(scene,m,clipped,s,{0,1024})->mean==
        estimate_volume_path_reference(scene,m,down,s,{0,1024})->mean);
    s.estimator=PathTraceEstimator::bsdf_only;
    statistical_check(population(scene,m,down,s,65536),Vec3d::Constant(expected));
}
TEST_CASE("volume finite sun energy overlap and opaque or mask shadow agree", "[volume]") {
    Fixture f; f.quad(100,0,{.5f,.5f,.5f}); EnvironmentDescription e; e.sun.direction={0,0,1}; e.sun.angular_radius_rad=.1;
    e.sun.irradiance={1,2,3}; f.document->set_environment(e).value(); auto s=settings(); s.sun=true; s.environment_rgb=Vec3d::Constant(.2);
    const Vec3d expected=Vec3d::Constant(.1)+Vec3d{1,2,3}*(.5/pi);
    statistical_check(population(f.scene(),medium(e),down,s,65536),expected);
    s.environment_rgb.setZero(); f.quad(100,2,{0,0,0},{0,0,0},true);
    CHECK(population(f.scene(),medium(e),down,s).mean.isZero(0));
    for(unsigned i=2;i<f.asset.materials.size();++i) { f.asset.materials[i].alpha_mode=AlphaMode::mask; f.asset.materials[i].base_color.w()=0; }
    statistical_check(population(f.scene(),medium(e),down,s),Vec3d{1,2,3}*(.5/pi));
    // A primary ray sees disk radiance directly; the outside ray sees only the independent boundary.
    Fixture empty; empty.document->set_environment(e).value(); s=settings(0); s.sun=true;
    const auto disk=solar_disk_radiance(e.sun).value();
    statistical_check(population(empty.scene(),medium(e),{{0,0,0},{0,0,1}},s),Vec3d{disk[0],disk[1],disk[2]});
    CHECK(population(empty.scene(),medium(e),{{0,0,0},{1,0,0}},s).mean.isZero(0));
}
TEST_CASE("volume pure absorption matches exponential RGB optical depth beyond camera far", "[volume]") {
    Fixture f; auto e=air(0,0); e.atmosphere.mie_extinction_per_m={2e-5,4e-5,6e-5}; f.document->set_environment(e).value();
    auto s=settings(0); s.environment_rgb={.3,.5,.8}; const auto m=medium(e); const auto scene=f.scene();
    Vec3d expected; for(int c=0;c<3;++c) expected[c]=s.environment_rgb[c]*std::exp(-e.atmosphere.mie_extinction_per_m[c]*10000*(std::exp(-.0001)-std::exp(-1.)));
    const auto p=population(scene,m,{{0,1,0},{0,1,0},.1,10},s,65536); statistical_check(p,expected);
    CHECK(p.stats.absorption>0); CHECK(p.stats.medium_scatter==0); CHECK(p.stats.null_collisions>0);
    const auto exact=estimate_volume_path_reference(scene,m,up,s,{0,4096}).value();
    const auto clipped=estimate_volume_path_reference(scene,m,{{0,1,0},{0,1,0},.1,10},s,{0,4096}).value();
    CHECK(exact.mean==clipped.mean);
}
TEST_CASE("volume single scattering agrees with independent spherical quadrature and N4N convergence", "[volume]") {
    Fixture f; auto e=air(5e-5,5e-5); e.sun.angular_radius_rad=.001; f.document->set_environment(e).value();
    auto s=settings(); s.sun=true; const auto scene=f.scene(); const auto m=medium(e);
    const double expected=single_scatter(e,2048); CHECK(std::abs(expected-single_scatter(e,512))<1e-6);
    const PathTraceRay view{{0,e.atmosphere.atmosphere_height_m,0},{0,-1,0}};
    // Sixteen independent pixel counters reduce hero-channel variance without changing seeds or tolerances.
    const auto n=population(scene,m,view,s,16384,16),four=population(scene,m,view,s,65536,16);
    statistical_check(n,Vec3d::Constant(expected)); statistical_check(four,Vec3d::Constant(expected));
    std::cout<<"single scatter N4N scalar="<<n.scalar<<' '<<four.scalar<<" difference="<<std::abs(n.scalar-four.scalar)/expected<<'\n';
    CHECK(std::abs(n.scalar-four.scalar)<=.005*expected);
    CHECK((n.mean-four.mean).cwiseAbs().maxCoeff()<=.005*expected);
    CHECK(four.stats.medium_scatter>0); CHECK(four.stats.absorption==0);
    // Geometry visibility applies at real medium vertices, including alpha-mask holes.
    const PathTraceRay inside{{0,9999,0},{0,-1,0}};
    const auto unoccluded=estimate_volume_path_reference(scene,m,inside,s,{0,4096}).value();
    f.triangle({-100000,10000,-100000},{100000,10000,-100000},{0,10000,100000},{0,0,0},{0,0,0},true);
    const auto occluded=estimate_volume_path_reference(f.scene(),m,inside,s,{0,4096}).value();
    CHECK(occluded.mean.isZero(0)); CHECK(occluded.statistics.medium_scatter>0);
    f.asset.materials[0].alpha_mode=AlphaMode::mask; f.asset.materials[0].base_color.w()=0;
    CHECK(estimate_volume_path_reference(f.scene(),m,inside,s,{0,4096})->mean==unoccluded.mean);
}
TEST_CASE("volume conservative multiple scattering furnace converges with MIS and roulette", "[volume]") {
    Fixture f; auto e=air(1e-5,1e-5); e.atmosphere.ground_albedo={1,1,1}; f.document->set_environment(e).value();
    const auto scene=f.scene(); const auto m=medium(e); auto s=settings(8); s.environment_rgb=Vec3d::Ones();
    const PathTraceRay ground{{0,1,0},{0,-1,0}};
    const auto shallow=population(scene,m,ground,s,16384); statistical_check(shallow,Vec3d::Ones());
    s.max_bounces=32; const auto deep=population(scene,m,ground,s,65536); statistical_check(deep,Vec3d::Ones());
    CHECK(std::abs(shallow.scalar-deep.scalar)<.005);
    CHECK(deep.stats.medium_scatter>20000); CHECK(deep.stats.absorption==0);
    CHECK(deep.stats.multiple_scatter_paths>1000);
    s.estimator=PathTraceEstimator::bsdf_only; statistical_check(population(scene,m,ground,s),Vec3d::Ones());
    s.estimator=PathTraceEstimator::mis; s.rr_start=2; statistical_check(population(scene,m,ground,s),Vec3d::Ones());
}
TEST_CASE("volume real cloud absorption and sun shadow agree with converged medium reference", "[volume]") {
    Fixture f; EnvironmentDescription e; e.clouds.enabled=true; e.clouds.coverage=1; e.sun.angular_radius_rad=1e-6;
    e.clouds.weather_resolution=e.clouds.base_resolution=e.clouds.detail_resolution=16;
    e.atmosphere.ground_albedo={.5,.5,.5}; f.document->set_environment(e).value();
    CloudFieldCache cache; const auto field=cache.prepare(e.clouds).value(); PathMediumSettings ms; ms.cloud_albedo=0; ms.time_seconds=17;
    const auto m=medium(e,ms,field); const auto scene=f.scene(); const PathTraceRay ray{{700,1,-500},{0,1,0}};
    const PathMediumRay shadow{ray.origin,ray.direction,100000};
    const auto reference=m.integrate(shadow,{8192,40960}).value();
    CHECK((reference.transmittance-m.integrate(shadow,{4096,40960})->transmittance).cwiseAbs().maxCoeff()<1e-4);
    REQUIRE(reference.transmittance.x()<.99); auto s=settings(0); s.environment_rgb=Vec3d::Ones();
    statistical_check(population(scene,m,ray,s,65536),reference.transmittance);
    s=settings(); s.sun=true;
    statistical_check(population(scene,m,{{700,1,-500},{0,-1,0}},s,65536),reference.transmittance*(.5/pi));
    // Actual cloud scattering, including repeated events, must conserve energy in a white furnace.
    e.atmosphere.ground_albedo={1,1,1}; f.document->set_environment(e).value(); ms.cloud_albedo=1;
    s=settings(32); s.environment_rgb=Vec3d::Ones();
    const auto scatter=population(f.scene(),medium(e,ms,field),ray,s,16384);
    statistical_check(scatter,Vec3d::Ones()); CHECK(scatter.stats.medium_scatter>1000); CHECK(scatter.stats.absorption==0);
    CHECK(scatter.stats.multiple_scatter_paths>100);
    auto mixed=air(1e-5,1e-5); mixed.clouds=e.clouds; mixed.atmosphere.ground_albedo={1,1,1};
    mixed.atmosphere.rayleigh_scattering_per_m={2e-6,3e-6,4e-6}; f.document->set_environment(mixed).value();
    statistical_check(population(f.scene(),medium(mixed,ms,field),ray,s),Vec3d::Ones());
}
TEST_CASE("volume boundaries order planet and mesh and reject underground camera", "[volume]") {
    Fixture f; auto e=air(0,0); f.document->set_environment(e).value();
    // Mesh is above the zero-coefficient planet and remains the nearer boundary.
    f.triangle({-10,1,-10},{0,1,10},{10,1,-10},{0,0,0},{1,1,1});
    const auto m=medium(e); auto s=settings(0); const PathTraceRay ray{{0,2,0},{0,-1,0}};
    statistical_check(population(f.scene(),m,ray,s),Vec3d::Ones());
    for(auto& p:f.asset.mesh.primitives) for(auto& v:p.positions) v.y()=-1;
    const auto scene=f.scene(); CHECK(population(scene,m,ray,s).mean.isZero(0));
    CHECK_FALSE(estimate_volume_path_reference(scene,m,{{0,-1,0},{0,1,0}},s,{0,1}));
    // No implicit planet in a vacuum, even for a mesh below the nominal ground.
    e.enabled=false; f.document->set_environment(e).value(); statistical_check(population(f.scene(),medium(e),ray,s),Vec3d::Ones());
    // At the largest supported planet radius, the far outer exit exceeds 2e7 m,
    // but the nearby ground still ends this physical path within the supported domain.
    e.enabled=true; e.atmosphere.ground_radius_m=1e7; e.atmosphere.planet_center_m={0,-1e7,0};
    e.atmosphere.atmosphere_height_m=1e6; f.document->set_environment(e).value();
    REQUIRE(estimate_volume_path_reference(f.scene(),medium(e),ray,s,{0,1}));
    e.atmosphere.planet_center_m={0,-5e7,0}; f.document->set_environment(e).value();
    // This distant planet requires a true unsupported segment; it must not become vacuum.
    f.asset.mesh.primitives.clear(); f.asset.materials.clear();
    const auto too_far=estimate_volume_path_reference(f.scene(),medium(e),ray,s,{0,1}); REQUIRE_FALSE(too_far);
    CHECK(too_far.error().code==ErrorCode::not_supported);
}
TEST_CASE("volume sample partitions and immutable image provenance preserve ownership", "[volume]") {
    Fixture f; f.quad(10,0,{.5f,.5f,.5f}); const auto source=f.source(); auto scene=f.scene(); auto m=medium(); auto s=settings();
    s.environment_rgb={1,.5,.25}; const auto view=f.view(source,2);
    const auto a=estimate_volume_path_reference(scene,m,down,s,{0,1024},7).value();
    const auto b=estimate_volume_path_reference(scene,m,down,s,{1024,1024},7).value();
    const auto all=estimate_volume_path_reference(scene,m,down,s,{0,2048},7).value();
    CHECK((all.mean-(a.mean+b.mean)*.5).norm()<1e-14); CHECK((all.second_moment-(a.second_moment+b.second_moment)*.5).norm()<1e-14);
    auto image=render_volume_path_reference(f.heap,scene,m,view,s,{0,128}).value();
    CHECK(image.info().scene_fingerprint==scene.fingerprint()); CHECK(image.info().environment==m.environment());
    CHECK(image.info().settings.model_version==4); CHECK(image.info().settings.sampler_version==2);
    CHECK(image.info().samples.count==128); CHECK(image.linear_rgb().size()==4); CHECK(image.second_moments().size()==4);
    const Vec3d old=image.linear_rgb()[0]; f.document.reset(); f.asset={}; f.heap.begin_close();
    CHECK_FALSE(render_volume_path_reference(f.heap,scene,m,view,s,{0,1}));
    scene={}; m={};
    CHECK(image.linear_rgb()[0]==old); CHECK(image.info().settings.environment_rgb==s.environment_rgb);
    CHECK_FALSE(render_volume_path_reference(f.heap,{}, {},view,s,{0,1}));
}
TEST_CASE("volume invalid settings and exhausted tracking reject complete candidates", "[volume]") {
    Fixture f; auto e=air(.001,0); f.document->set_environment(e).value(); const auto scene=f.scene(); const auto m=medium(e);
    auto s=settings(); s.environment_rgb=Vec3d::Ones();
    auto mismatch=e; mismatch.sun.irradiance={2,2,2}; CHECK_FALSE(estimate_volume_path_reference(scene,medium(mismatch),up,s,{0,1}));
    CHECK_FALSE(estimate_volume_path_reference({},m,up,s,{0,1}));
    for(unsigned mode=0;mode<10;++mode) {
        auto bad=s;
        switch(mode) {
        case 0: bad.model_version=3; break; case 1: bad.sampler_version=1; break; case 2: bad.max_bounces=33; break;
        case 3: bad.rr_start=2; break; case 4: bad.environment_rgb.x()=-1; break; case 5: bad.max_rays=1; break;
        case 6: bad.max_triangle_tests=0; break; case 7: bad.max_tracking_steps=1ull<<29; break;
        case 8: bad.max_steps_per_path=0; break; case 9: bad.estimator=PathTraceEstimator(10); break;
        }
        CHECK_FALSE(estimate_volume_path_reference(scene,m,up,bad,{0,16}));
    }
    CHECK_FALSE(estimate_volume_path_reference(scene,m,up,s,{65535,2}));
    CHECK_FALSE(estimate_volume_path_reference(scene,m,up,s,{0,0}));
    s.max_tracking_steps=1; auto failed=estimate_volume_path_reference(scene,m,up,s,{0,128}); REQUIRE_FALSE(failed);
    CHECK(failed.error().message.find("tracking")!=std::string::npos);
    s.max_tracking_steps=1ull<<26; s.max_steps_per_path=1;
    CHECK_FALSE(estimate_volume_path_reference(scene,m,up,s,{0,128}));
    std::stop_source cancel; cancel.request_stop();
    auto stopped=estimate_volume_path_reference(scene,m,up,settings(),{0,1},0,cancel.get_token()); REQUIRE_FALSE(stopped);
    CHECK(stopped.error().context==std::vector<std::string>{"render.pathtracing.volume.cancelled"});
    REQUIRE(estimate_volume_path_reference(scene,m,up,settings(),{0,1}));
}
TEST_CASE("volume allocation failure and active cancellation release partial images", "[volume]") {
    Fixture f; f.quad(100,0,{.8f,.8f,.8f}); const auto scene=f.scene(); const auto source=f.source(); const auto m=medium(); auto s=settings(); s.environment_rgb=Vec3d::Ones();
    const auto view=f.view(source); const auto small=f.system.create_heap({"volume-budget",memory::DomainCategory::render,32768}).value();
    std::vector<void*> blocks; while(auto p=small.try_allocate(256)) blocks.push_back(*p);
    bool failed=false,partial=false,success=false;
    for(;;) {
        const auto before=small.snapshot();
        { auto image=render_volume_path_reference(small,scene,m,view,s,{0,1}); failed|=!image; success|=bool(image);
          if(!image) partial|=small.snapshot().allocation_count>before.allocation_count+1; }
        CHECK(small.snapshot().live_allocations==before.live_allocations);
        CHECK(small.snapshot().backing_requested_bytes==before.backing_requested_bytes);
        if(blocks.empty()) break; const auto p=blocks.back(); blocks.pop_back(); small.deallocate(p,256);
    }
    CHECK(failed); CHECK(partial); CHECK(success);
    const auto before=f.heap.snapshot(); std::stop_source cancel;
    std::jthread stopper([&] { std::this_thread::sleep_for(std::chrono::milliseconds(10)); cancel.request_stop(); });
    auto cancelled=render_volume_path_reference(f.heap,scene,m,f.view(source,16),s,{0,16384},cancel.get_token());
    REQUIRE_FALSE(cancelled); CHECK(cancelled.error().context==std::vector<std::string>{"render.pathtracing.volume.cancelled"});
    CHECK(f.heap.snapshot().live_allocations==before.live_allocations);
    REQUIRE(render_volume_path_reference(f.heap,scene,m,view,s,{0,1}));
}
TEST_CASE("volume frozen cloud image owners budgets and tracking cancellation remain isolated", "[volume]") {
    Fixture f; auto e=air(1e-5,1e-5); e.clouds.enabled=true; e.clouds.coverage=1;
    e.clouds.weather_resolution=e.clouds.base_resolution=e.clouds.detail_resolution=16;
    f.document->set_environment(e).value(); const auto scene=f.scene(); const auto source=f.source();
    auto description=f.view(source,4).description(); Trsd camera; camera.translation={0,2000,0};
    description.camera_world=Transformd::from_trs(camera).value(); const auto view=RenderView::create(source,description).value();
    auto cache=std::make_unique<CloudFieldCache>(); auto field=cache->prepare(e.clouds).value(); std::weak_ptr<const CloudFieldData> weak=field;
    PathMediumSettings ms; ms.time_seconds=23; auto m=medium(e,ms,field); auto s=settings(8); s.sun=true;
    auto old=render_volume_path_reference(f.heap,scene,m,view,s,{0,16}).value(); const Vec3d color=old.linear_rgb()[0];
    const auto before=f.heap.snapshot(); auto bad=s; bad.max_tracking_steps=1;
    auto rejected=render_volume_path_reference(f.heap,scene,m,view,bad,{0,16}); REQUIRE_FALSE(rejected);
    CHECK(rejected.error().message.find("tracking")!=std::string::npos);
    CHECK(f.heap.snapshot().live_allocations==before.live_allocations); CHECK(old.linear_rgb()[0]==color);
    std::stop_source cancel;
    std::jthread stopper([&] { std::this_thread::sleep_for(std::chrono::milliseconds(10)); cancel.request_stop(); });
    auto interrupted=render_volume_path_reference(f.heap,scene,m,view,s,{0,32768},cancel.get_token()); REQUIRE_FALSE(interrupted);
    CHECK(interrupted.error().context==std::vector<std::string>{"render.pathtracing.volume.cancelled"});
    CHECK(f.heap.snapshot().live_allocations==before.live_allocations);
    ++e.clouds.seed; REQUIRE(cache->prepare(e.clouds)); f.document->set_environment(e).value();
    field.reset(); cache.reset(); m={}; CHECK_FALSE(weak.expired());
    CHECK(old.info().field_hash!=0); CHECK(old.info().medium.time_seconds==23); CHECK(old.info().environment.clouds.seed==1);
    CHECK(old.linear_rgb()[0]==color); old={}; CHECK(weak.expired());
}

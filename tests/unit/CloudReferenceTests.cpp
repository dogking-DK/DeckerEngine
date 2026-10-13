#include "../fixtures/CloudReferenceCases.hpp"
#include <catch2/catch_test_macros.hpp>
#include <cstdio>
#include <limits>
#include <numbers>
using namespace dk;
using namespace dk::render;
namespace {
EnvironmentDescription homogeneous() { auto e=cloud_cases::environment(); e.enabled=false; e.sun.direction={0,1,0}; return e; }
CloudReferenceSettings constant() { CloudReferenceSettings s; s.homogeneous=true; s.self_shadow=false; s.multiple_scattering=0;
    s.phase_g=0; s.steps=32; s.sun_steps=s.atmosphere_steps=8; s.sky_radiance={.1,.2,.3}; return s; }
CloudIntegral integrate(const EnvironmentDescription& e,const CloudFieldData& f,const CloudRay& r,const CloudReferenceSettings& s) {
    auto result=integrate_cloud_reference(e,f,r,s); REQUIRE(result); return *result;
}
void bounded(const CloudIntegral& value) {
    CHECK(value.radiance.allFinite()); CHECK((value.radiance.array()>=0).all()); CHECK(std::isfinite(value.transmittance));
    CHECK(value.transmittance>=0); CHECK(value.transmittance<=1); CHECK(std::isfinite(value.mean_distance_m));
}
}
TEST_CASE("reference shell clips ground finite rays and both sides without tangent volume", "[clouds][reference]") {
    auto e=homogeneous();
    auto path=cloud_shell_intervals(e,{{0,0,0},{0,1e300,0},10000}).value();
    REQUIRE(path.count==1); CHECK(path.segments[0].begin_m==1500); CHECK(path.segments[0].end_m==4000);
    path=cloud_shell_intervals(e,{{0,2000,0},{0,1,0},800}).value();
    REQUIRE(path.count==1); CHECK(path.segments[0].begin_m==0); CHECK(path.segments[0].end_m==800);
    path=cloud_shell_intervals(e,{{0,6000,0},{0,-1,0},20000}).value();
    REQUIRE(path.count==1); CHECK(path.ground); CHECK(path.segments[0].begin_m==2000); CHECK(path.segments[0].end_m==4500);
    path=cloud_shell_intervals(e,{{0,6000,0},{0,1,0},20000}).value(); CHECK(path.count==0);
    path=cloud_shell_intervals(e,{{0,4000,0},{1,0,0},20000}).value(); CHECK(path.count==0);
    path=cloud_shell_intervals(e,{{0,1500,0},{1,0,0},20000}).value(); CHECK(path.count==1);
    path=cloud_shell_intervals(e,{{0,0,0},{0,-1,0},20000}).value(); CHECK(path.count==0); CHECK(path.ground);
    e.atmosphere.planet_center_m={0,0,0}; e.atmosphere.ground_radius_m=1000;
    e.clouds.bottom_height_m=100; e.clouds.top_height_m=200;
    path=cloud_shell_intervals(e,{{-2000,1050,0},{1,0,0},4000}).value();
    REQUIRE(path.count==2); CHECK_FALSE(path.ground);
    const double outer=std::sqrt(1200.*1200-1050.*1050),inner=std::sqrt(1100.*1100-1050.*1050);
    CHECK(std::abs(path.segments[0].begin_m-(2000-outer))<1e-10);
    CHECK(std::abs(path.segments[0].end_m-(2000-inner))<1e-10);
    CHECK(std::abs(path.segments[1].begin_m-(2000+inner))<1e-10);
    CHECK(std::abs(path.segments[1].end_m-(2000+outer))<1e-10);
    CHECK_FALSE(cloud_shell_intervals(e,{{0,0,0},{0,1,0},1000}));
}
TEST_CASE("reference Beer Lambert matches analytic thin thick and pure absorption", "[clouds][reference]") {
    auto e=homogeneous(); auto s=constant(); CloudFieldCache cache; auto f=cache.prepare(e.clouds).value();
    const CloudRay ray{{0,0,0},{0,1,0},10000};
    for(double extinction:{1e-12,1e-5,.001,1.}) {
        e.clouds.extinction_per_m=extinction;
        const double trans=std::exp(-extinction*2500),opacity=-std::expm1(-extinction*2500);
        const Vec3d source=s.sky_radiance+Vec3d{e.sun.irradiance.data()}/(4*std::numbers::pi);
        for(unsigned steps:{1u,32u,256u}) {
            s.steps=steps; const auto value=integrate(e,*f,ray,s); bounded(value);
            CHECK(std::abs(value.transmittance-trans)<=1e-10);
            CHECK((value.radiance-source*(s.single_scattering_albedo*opacity)).cwiseAbs().maxCoeff()<=1e-10);
            CHECK(value.distance_m==2500); CHECK(value.samples==steps); CHECK(value.mean_distance_m>=1500); CHECK(value.mean_distance_m<=4000);
        }
    }
    s.single_scattering_albedo=0; const auto absorbed=integrate(e,*f,ray,s); CHECK(absorbed.radiance.isZero(0)); CHECK(absorbed.transmittance<1e-10);
}
TEST_CASE("reference zero controls diagnostics validation and explicit time are deterministic", "[clouds][reference]") {
    auto e=homogeneous(); auto s=constant(); CloudFieldCache cache; auto f=cache.prepare(e.clouds).value(); const CloudRay ray;
    for(unsigned mode=0;mode<4;++mode) {
        auto zero=e; if(mode==0) zero.clouds.enabled=false; if(mode==1) zero.clouds.coverage=0;
        if(mode==2) zero.clouds.density=0; if(mode==3) zero.clouds.extinction_per_m=0;
        const auto value=integrate(zero,*f,ray,s); CHECK(value.radiance.isZero(0)); CHECK(value.transmittance==1); CHECK(value.samples==0);
    }
    e.sun.irradiance={0,0,0}; s.sky_radiance.setZero(); CHECK(integrate(e,*f,ray,s).radiance.isZero(0));
    auto bad=s; bad.steps=0; CHECK_FALSE(integrate_cloud_reference(e,*f,ray,bad));
    bad=s; bad.sun_steps=513; CHECK_FALSE(integrate_cloud_reference(e,*f,ray,bad));
    bad=s; bad.atmosphere_steps=513; CHECK_FALSE(integrate_cloud_reference(e,*f,ray,bad));
    bad=s; bad.max_work=1; CHECK_FALSE(integrate_cloud_reference(e,*f,ray,bad));
    bad=s; bad.multiple_scattering=.51; CHECK_FALSE(integrate_cloud_reference(e,*f,ray,bad));
    bad=s; bad.phase_g=1; CHECK_FALSE(integrate_cloud_reference(e,*f,ray,bad));
    bad=s; bad.sky_radiance.x()=std::numeric_limits<double>::infinity(); CHECK_FALSE(integrate_cloud_reference(e,*f,ray,bad));
    bad=s; bad.model_version=2; CHECK_FALSE(integrate_cloud_reference(e,*f,ray,bad));
    bad=s; bad.time_seconds=1e10; CHECK_FALSE(integrate_cloud_reference(e,*f,ray,bad));
    CHECK_FALSE(integrate_cloud_reference(e,*f,{{0,0,0},{0,0,0},1},s));
    CHECK_FALSE(integrate_cloud_reference(e,*f,{{0,0,0},{0,1,0},std::numeric_limits<double>::infinity()},s));
    ++e.clouds.seed; CHECK_FALSE(integrate_cloud_reference(e,*f,ray,s));
    e=cloud_cases::environment(); s=cloud_cases::settings(); s.steps=32; s.sun_steps=s.atmosphere_steps=8;
    const auto first=integrate(e,*f,ray,s); s.time_seconds=-400; const auto changed=integrate(e,*f,ray,s); s.time_seconds=17.25;
    const auto restored=integrate(e,*f,ray,s); CHECK(first.radiance==restored.radiance); CHECK(first.transmittance==restored.transmittance);
    CHECK(std::abs(changed.transmittance-first.transmittance)>1e-5);
    ++e.clouds.seed; auto other=cache.prepare(e.clouds).value(); const auto seeded=integrate(e,*other,ray,s);
    CHECK(std::abs(seeded.transmittance-first.transmittance)>1e-5); CHECK(f->hash()==0xc859ef6a9ec30856ULL);
}
TEST_CASE("reference lighting separates shadow phase air and normalized multiple scattering", "[clouds][reference]") {
    auto e=homogeneous(); auto s=constant(); s.sky_radiance.setZero(); s.phase_g=.6; s.steps=256; s.sun_steps=64;
    CloudFieldCache cache; auto f=cache.prepare(e.clouds).value(); const CloudRay up{{0,2200,0},{0,1,0},800},down{{0,3000,0},{0,-1,0},800};
    const auto forward=integrate(e,*f,up,s),backward=integrate(e,*f,down,s); CHECK(forward.radiance.x()>backward.radiance.x());
    s.self_shadow=true; const auto shadowed=integrate(e,*f,up,s); CHECK(shadowed.radiance.x()<forward.radiance.x());
    s.multiple_scattering=.35; const auto multiple=integrate(e,*f,up,s); bounded(multiple);
    CHECK(std::abs(multiple.radiance.x()-shadowed.radiance.x())>1e-4);
    CHECK(multiple.radiance.x()<s.single_scattering_albedo*(1-multiple.transmittance)*mie_phase(1,s.phase_g));
    e.enabled=true; const auto air=integrate(e,*f,up,s); CHECK((air.radiance.array()<multiple.radiance.array()).all());
    e.sun.direction={0,-1,0}; for(bool self:{false,true}) { s.self_shadow=self; CHECK(integrate(e,*f,up,s).radiance.isZero(0)); }
    // Independently integrate the angular probability; redistribution cannot add total phase energy.
    double total=0; constexpr unsigned n=20000;
    for(unsigned i=0;i<n;++i) { const double mu=-1+(i+.5)*2/n;
        total+=((1-.35)*mie_phase(mu,.6)+.35*(2./3*mie_phase(mu,.3)+1./3*mie_phase(mu,.15)))*4*std::numbers::pi/n; }
    CHECK(std::abs(total-1)<1e-6);
}
TEST_CASE("reference real fields converge with model approximation held fixed", "[clouds][reference]") {
    const auto e=cloud_cases::environment(); CloudFieldCache cache; auto f=cache.prepare(e.clouds).value(); const auto rays=cloud_cases::rays();
    for(unsigned i=0;i<rays.size();++i) {
        const auto low=integrate(e,*f,rays[i],cloud_cases::settings(0));
        const auto middle=integrate(e,*f,rays[i],cloud_cases::settings(1));
        const auto high=integrate(e,*f,rays[i],cloud_cases::settings(2)); bounded(high);
        std::printf("cloud convergence ray=%u 256_to_512 T=%.9g RGB=%.9g 512_to_1024 T=%.9g RGB=%.9g final_T=%.9g L=(%.9g,%.9g,%.9g)\n",
            i,std::abs(low.transmittance-middle.transmittance),cloud_cases::rgb_error(low,middle),std::abs(middle.transmittance-high.transmittance),
            cloud_cases::rgb_error(middle,high),high.transmittance,high.radiance.x(),high.radiance.y(),high.radiance.z());
        CHECK(high.transmittance<.99); CHECK(high.radiance.maxCoeff()>.001);
        CHECK(std::abs(middle.transmittance-high.transmittance)<=.005); CHECK(cloud_cases::rgb_error(middle,high)<=.02);
    }
}
TEST_CASE("reference sunlight accepts thin atmosphere independent of default cloud heights", "[clouds][reference]") {
    auto e=homogeneous(); e.enabled=true; e.atmosphere.atmosphere_height_m=3000;
    e.atmosphere.rayleigh_scale_height_m=1000; e.atmosphere.mie_scale_height_m=500;
    e.atmosphere.absorption_center_height_m=e.atmosphere.absorption_half_width_m=1000;
    e.clouds.bottom_height_m=1000; e.clouds.top_height_m=2000;
    CloudFieldCache cache; auto f=cache.prepare(e.clouds).value(); const auto s=constant();
    const auto air=integrate(e,*f,{},s); bounded(air); CHECK(air.radiance.maxCoeff()>0);
    const auto trans=atmosphere_transmittance_reference(e.atmosphere,1000,1,64);
    REQUIRE(trans); CHECK((trans->array()>0).all()); CHECK((trans->array()<1).all());
    e.atmosphere.rayleigh_scale_height_m=4000; CHECK_FALSE(atmosphere_transmittance_reference(e.atmosphere,1000,1,64));
}
TEST_CASE("composition mixed absorption follows analytic optical depth and terminal clipping", "[clouds][composition]") {
    auto e=homogeneous(); e.enabled=true; e.sun.irradiance={0,0,0};
    e.atmosphere.rayleigh_scattering_per_m={}; e.atmosphere.mie_scattering_per_m={}; e.atmosphere.mie_extinction_per_m={};
    e.atmosphere.absorption_center_height_m=0; e.atmosphere.absorption_half_width_m=10000;
    e.atmosphere.absorption_extinction_per_m={1e-4,2e-4,3e-4}; e.clouds.extinction_per_m=.0003;
    auto s=constant(); s.sky_radiance.setZero(); s.single_scattering_albedo=0;
    CloudFieldCache cache; const auto f=cache.prepare(e.clouds).value();
    for(double limit:{0.,1000.,2000.,6000.}) {
        auto value=integrate_cloud_medium_reference(e,*f,{{0,0,0},{0,1,0},limit},s); REQUIRE(value);
        const double cloud_distance=std::max(0.,std::min(4000.,limit)-1500.);
        const double air_length=limit-limit*limit/20000;
        for(unsigned c=0;c<3;++c) CHECK(std::abs(value->transmittance[c]-std::exp(-air_length*e.atmosphere.absorption_extinction_per_m[c]-cloud_distance*.0003))<1e-10);
        CHECK(std::abs(value->cloud_transmittance-std::exp(-cloud_distance*.0003))<1e-10); CHECK(value->radiance.isZero(0));
        CHECK_FALSE(value->ground);
    }
    auto ground=integrate_cloud_medium_reference(e,*f,{{0,2000,0},{0,-1,0},5000},s); REQUIRE(ground); CHECK(ground->ground);
    CloudCompositionSettings image; CHECK(validate_cloud_composition(e,image,4096)); CHECK_FALSE(validate_cloud_composition(e,image,4097));
    image.max_work=1; CHECK_FALSE(validate_cloud_composition(e,image,1));
    s.max_work=std::uint64_t{2}*s.steps*(2*s.sun_steps+s.atmosphere_steps+1);
    CHECK_FALSE(integrate_cloud_medium_reference(e,*f,{},s));
}
TEST_CASE("composition air and cloud share attenuation without duplicating sources", "[clouds][composition]") {
    auto e=homogeneous(); const auto s=constant(); CloudFieldCache cache; const auto f=cache.prepare(e.clouds).value();
    const CloudRay ray{{0,100,0},{0,1,0},6000};
    const auto cloud=integrate_cloud_reference(e,*f,ray,s).value();
    const auto mixed=integrate_cloud_medium_reference(e,*f,ray,s).value();
    CHECK((mixed.radiance-cloud.radiance).norm()<1e-12); CHECK((mixed.cloud_radiance-cloud.radiance).norm()<1e-12);
    CHECK((mixed.transmittance.array()-cloud.transmittance).abs().maxCoeff()<1e-12);
    e.enabled=true; e.clouds.extinction_per_m=0; auto high=s; high.steps=512; high.atmosphere_steps=256;
    const auto only_air=integrate_cloud_medium_reference(e,*f,ray,high).value();
    const auto expected=integrate_atmosphere_reference(e,ray.origin_m,ray.direction,{1024,256,ray.max_distance_m,false}).value();
    CHECK((only_air.radiance-expected.radiance).cwiseAbs().maxCoeff()<1e-5);
    CHECK((only_air.transmittance-expected.transmittance).cwiseAbs().maxCoeff()<1e-5); CHECK(only_air.cloud_radiance.isZero(0));
    e.clouds.extinction_per_m=.001; e.sun.irradiance={0,0,0};
    const auto air=integrate_cloud_medium_reference(e,*f,ray,s).value(); e.enabled=false;
    const auto vacuum=integrate_cloud_medium_reference(e,*f,ray,s).value();
    CHECK((air.cloud_radiance.array()<vacuum.cloud_radiance.array()).all()); CHECK(air.radiance==air.cloud_radiance);
}

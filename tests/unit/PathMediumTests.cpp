#include <dk/render/PathMediumReference.hpp>
#if DK_PATH_MEDIUM_CLOUD_REFERENCE
#include <dk/render/CloudReference.hpp>
#endif
#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <cmath>
#include <limits>
using namespace dk;
using namespace dk::render;
namespace {
EnvironmentDescription air() { EnvironmentDescription e; e.enabled=true; return e; }
EnvironmentDescription clouds() {
    auto e=air(); e.clouds.enabled=true; e.clouds.coverage=1;
    e.clouds.weather_resolution=e.clouds.base_resolution=e.clouds.detail_resolution=16;
    return e;
}
PathMediumSnapshot snapshot(const EnvironmentDescription& e,const PathMediumSettings& s={},std::shared_ptr<const CloudFieldData> field={}) {
    auto value=PathMediumSnapshot::create(e,s,std::move(field)); REQUIRE(value); return *value;
}
PathMediumTransmittance integrate(const PathMediumSnapshot& medium,const PathMediumRay& ray,std::uint32_t steps=2048) {
    auto value=medium.integrate(ray,{steps,40960}); REQUIRE(value); return *value;
}
double difference(const Vec3d& a,const Vec3d& b) { return (a-b).cwiseAbs().maxCoeff(); }
}
TEST_CASE("medium vertical air matches independent exponential and triangular optical depths", "[pathtracing][medium]") {
    auto e=air(); const auto& a=e.atmosphere; const auto medium=snapshot(e);
    for(const double distance:{.001,1.,1000.,50000.,100000.}) {
        const auto value=integrate(medium,{{0,0,0},{0,1,0},distance});
        const double center=a.absorption_center_height_m,width=a.absorption_half_width_m;
        auto primitive=[&](double h) {
            const double left=std::clamp(h-(center-width),0.,width),right=std::clamp(h-center,0.,width);
            return left*left/(2*width)+right-right*right/(2*width);
        };
        Vec3d expected;
        for(int c=0;c<3;++c) expected[c]=a.rayleigh_scattering_per_m[c]*a.rayleigh_scale_height_m*(-std::expm1(-distance/a.rayleigh_scale_height_m))+
            a.mie_extinction_per_m[c]*a.mie_scale_height_m*(-std::expm1(-distance/a.mie_scale_height_m))+
            a.absorption_extinction_per_m[c]*(primitive(distance)-primitive(0));
        CHECK(difference(value.optical_depth,expected)<2e-6);
        CHECK(difference(value.transmittance,(-expected.array()).exp().matrix())<2e-6);
        CHECK_FALSE(value.ground);
    }
    e.atmosphere.rayleigh_scattering_per_m=e.atmosphere.mie_scattering_per_m={0,0,0};
    const auto absorbed=integrate(snapshot(e),{{0,0,0},{0,1,0},100000});
    CHECK((absorbed.transmittance.array()<1).all());
    auto point=snapshot(e).sample({0,100,0}).value();
    CHECK(point.rayleigh_scattering.isZero(0)); CHECK(point.mie_scattering.isZero(0));
}
TEST_CASE("medium intervals clip ground finite rays and both cloud shell crossings", "[pathtracing][medium]") {
    auto e=clouds(); CloudFieldCache cache; auto field=cache.prepare(e.clouds).value(); auto medium=snapshot(e,{},field);
    auto path=medium.intervals({{0,1.8,0},{0,1e300,0},10000}).value();
    REQUIRE(path.count==3); CHECK_FALSE(path.ground);
    CHECK(std::abs(path.segments[0].end_m-1498.2)<1e-8);
    CHECK(path.segments[1].clouds); CHECK(std::abs(path.segments[1].end_m-3998.2)<1e-8);
    CHECK(path.segments[2].end_m==10000);
    path=medium.intervals({{0,1.8,0},{0,-1,0},10000}).value();
    REQUIRE(path.ground); CHECK(std::abs(path.ground_distance_m-1.8)<1e-8);
    const auto down=integrate(medium,{{0,1.8,0},{0,-1,0},10000});
    CHECK(down.ground); CHECK(down.transmittance.minCoeff()>.99); // Ground opacity is not medium extinction.
    CHECK_FALSE(medium.intervals({{0,1.8,0},{0,-1,0},1}).value().ground);
    CHECK(medium.intervals({{0,0,0},{0,-1,0},1000}).value().ground);
    CHECK(medium.intervals({{0,1.8,0},{0,-1,0},0}).value().count==0);
    CHECK(medium.intervals({{0,120000,0},{0,1,0},10000}).value().count==0);
    e.atmosphere.planet_center_m={0,0,0}; e.atmosphere.ground_radius_m=1000;
    e.clouds.bottom_height_m=100; e.clouds.top_height_m=200;
    medium=snapshot(e,{},field); path=medium.intervals({{-2000,1050,0},{1,0,0},4000}).value();
    REQUIRE(path.count==5); CHECK_FALSE(path.ground);
    CHECK(path.segments[1].clouds); CHECK_FALSE(path.segments[2].clouds); CHECK(path.segments[3].clouds);
    const double outer=std::sqrt(1200.*1200-1050.*1050),inner=std::sqrt(1100.*1100-1050.*1050);
    CHECK(std::abs(path.segments[1].begin_m-(2000-outer))<1e-10);
    CHECK(std::abs(path.segments[1].end_m-(2000-inner))<1e-10);
    e.enabled=false; medium=snapshot(e,{},field);
    path=medium.intervals({{-2000,1200,0},{1,0,0},4000}).value(); CHECK(path.count==0);
    path=medium.intervals({{-2000,1100,0},{1,0,0},4000}).value();
    double length=0; for(unsigned i=0;i<path.count;++i) length+=path.segments[i].end_m-path.segments[i].begin_m;
    CHECK(std::abs(length-2*std::sqrt(1200.*1200-1100.*1100))<1e-10);
}
TEST_CASE("medium reversal segmentation and translation preserve RGB transmission", "[pathtracing][medium]") {
    auto e=air(); auto medium=snapshot(e); const Vec3d start{30,1.8,-20},direction=Vec3d{.2,.7,.3}.normalized();
    const double distance=13000,cut=3700; const Vec3d end=start+direction*distance;
    auto full=integrate(medium,{start,direction,distance});
    auto backward=integrate(medium,{end,-direction,distance});
    CHECK(difference(full.transmittance,backward.transmittance)<1e-10);
    auto first=integrate(medium,{start,direction,cut}),second=integrate(medium,{start+direction*cut,direction,distance-cut});
    CHECK(difference(full.transmittance,first.transmittance.cwiseProduct(second.transmittance))<2e-6);
    const Vec3d shift{20000000,-30000000,40000000};
    for(int i=0;i<3;++i) e.atmosphere.planet_center_m[i]+=shift[i];
    CHECK(difference(full.transmittance,integrate(snapshot(e),{start+shift,direction,distance}).transmittance)<1e-10);
}
TEST_CASE("medium coefficients retain scattering absorption and conservative majorants", "[pathtracing][medium]") {
    auto e=clouds(); CloudFieldCache cache; auto field=cache.prepare(e.clouds).value(); const auto medium=snapshot(e,{},field);
    bool saw_cloud=false;
    for(int i=0;i<1024;++i) {
        const Vec3d p{double(i%32)*713,1500.+double(i/32)*79,double(i%17)*-391};
        auto coefficient=medium.sample(p); REQUIRE(coefficient);
        CHECK((coefficient->extinction.array()<=medium.extinction_majorant().array()).all());
        CHECK((coefficient->extinction.array()>=0).all());
        CHECK(((coefficient->rayleigh_scattering+coefficient->mie_scattering).array()+coefficient->cloud_scattering<=coefficient->extinction.array()+1e-16).all());
        saw_cloud|=coefficient->cloud_scattering>0;
    }
    CHECK(saw_cloud);
    const auto surface=medium.sample({0,0,0}).value();
    for(int i=0;i<3;++i) {
        CHECK(surface.rayleigh_scattering[i]==e.atmosphere.rayleigh_scattering_per_m[i]);
        CHECK(surface.mie_scattering[i]==e.atmosphere.mie_scattering_per_m[i]);
    }
    CHECK(medium.sample({0,100000,0}).value().extinction.isZero(0));
    auto no_scatter=medium.settings(); no_scatter.cloud_albedo=0;
    auto absorbing=snapshot(e,no_scatter,field);
    auto a=medium.sample({700,2300,-500}).value(),b=absorbing.sample({700,2300,-500}).value();
    CHECK(a.extinction==b.extinction); CHECK(b.cloud_scattering==0);
}
TEST_CASE("medium vacuum and zero cloud controls need no field", "[pathtracing][medium]") {
    EnvironmentDescription e; auto vacuum=snapshot(e);
    auto result=integrate(vacuum,{{0,1,0},{0,-1,0},1000});
    CHECK(result.transmittance==Vec3d::Ones()); CHECK(result.optical_depth.isZero(0));
    CHECK(result.samples==0); CHECK_FALSE(result.ground); CHECK(vacuum.extinction_majorant().isZero(0));
    for(unsigned mode=0;mode<4;++mode) {
        e=clouds(); if(mode==0) e.clouds.enabled=false; if(mode==1) e.clouds.coverage=0;
        if(mode==2) e.clouds.density=0; if(mode==3) e.clouds.extinction_per_m=0;
        auto value=snapshot(e); CHECK(value.field_hash()==0);
        CHECK(integrate(value,{{0,0,0},{0,1,0},10000}).transmittance==integrate(snapshot(air()),{{0,0,0},{0,1,0},10000}).transmittance);
    }
}
TEST_CASE("medium frozen time and field owner survive source changes and failed preparation", "[pathtracing][medium]") {
    auto e=clouds(); e.clouds.wind_velocity_mps={100,-50}; CloudFieldCache cache; auto field=cache.prepare(e.clouds).value();
    PathMediumSettings settings; settings.time_seconds=17.25;
    const auto old=snapshot(e,settings,field); const PathMediumRay ray{{0,1600,0},{.5,1,-.2},4000};
    const auto before=integrate(old,ray,1024);
    settings.time_seconds=500;
    CHECK(difference(before.transmittance,integrate(snapshot(e,settings,field),ray,1024).transmittance)>1e-5);
    ++e.clouds.seed;
    CHECK_FALSE(PathMediumSnapshot::create(e,settings,field));
    auto next=cache.prepare(e.clouds); REQUIRE(next); CHECK((*next)->hash()!=old.field_hash());
    field.reset(); next={};
    CHECK(integrate(old,ray,1024).transmittance==before.transmittance);
    CHECK(old.settings().time_seconds==17.25); CHECK(old.environment().clouds.seed==1);
    settings.cloud_albedo=2; CHECK_FALSE(PathMediumSnapshot::create(e,settings,cache.current()));
    CHECK(integrate(old,ray,1024).transmittance==before.transmittance);
}
TEST_CASE("medium rejects invalid inputs budgets and cancellation without partial publication", "[pathtracing][medium]") {
    auto e=clouds(); CloudFieldCache cache; auto field=cache.prepare(e.clouds).value(); auto medium=snapshot(e,{},field);
    CHECK_FALSE(PathMediumSnapshot::create(e));
    CHECK_FALSE(PathMediumSnapshot{}.sample({0,1,0}));
    CHECK_FALSE(PathMediumSnapshot{}.intervals({}));
    for(const Vec3d p:{Vec3d{0,-1,0},Vec3d{1e10,0,0},Vec3d{0,std::numeric_limits<double>::quiet_NaN(),0}})
        CHECK_FALSE(medium.sample(p));
    for(const PathMediumRay r:{PathMediumRay{{0,-1,0},{0,1,0},1},PathMediumRay{{0,0,0},{0,0,0},1},
        PathMediumRay{{0,0,0},{0,1,0},-1},PathMediumRay{{0,0,0},{0,1,0},std::numeric_limits<double>::infinity()},
        PathMediumRay{{1e9,0,0},{1,0,0},1},PathMediumRay{{0,0,0},{0,std::numeric_limits<double>::infinity(),0},1}})
        CHECK_FALSE(medium.integrate(r));
    for(const PathMediumIntegration s:{PathMediumIntegration{0,40960},PathMediumIntegration{8193,40960},
        PathMediumIntegration{512,0},PathMediumIntegration{512,40961},PathMediumIntegration{512,511}})
        CHECK_FALSE(medium.integrate({{0,0,0},{0,1,0},10000},s));
    std::stop_source stop; stop.request_stop(); auto stopped=medium.integrate({}, {},stop.get_token());
    REQUIRE_FALSE(stopped); CHECK(stopped.error().message=="Path medium integration cancelled");
    CHECK(medium.integrate({}));
    PathMediumSettings s; s.model_version=2; CHECK_FALSE(PathMediumSnapshot::create(e,s,field));
    s={}; s.time_seconds=1e10; CHECK_FALSE(PathMediumSnapshot::create(e,s,field));
    s={}; s.cloud_anisotropy=1; CHECK_FALSE(PathMediumSnapshot::create(e,s,field));
    e.atmosphere.ground_radius_m=1e8; CHECK_FALSE(PathMediumSnapshot::create(e,{},field));
}
#if DK_PATH_MEDIUM_CLOUD_REFERENCE
TEST_CASE("medium real cloud transmission agrees with independent M15 reference and mixed extinction", "[pathtracing][medium]") {
    auto e=clouds(); CloudFieldCache cache; auto field=cache.prepare(e.clouds).value();
    PathMediumSettings s; s.time_seconds=17.25; const auto mixed=snapshot(e,s,field);
    auto air_only=e; air_only.clouds.enabled=false;
    auto cloud_only=e; cloud_only.enabled=false; const auto cloud=snapshot(cloud_only,s,field);
    CloudReferenceSettings reference; reference.time_seconds=s.time_seconds; reference.steps=1024;
    reference.sun_steps=reference.atmosphere_steps=1; reference.self_shadow=false; reference.multiple_scattering=0; reference.single_scattering_albedo=0;
    for(const PathMediumRay ray:{PathMediumRay{{0,100,0},{0,1,0},20000},PathMediumRay{{1300,2200,-700},{.7,.25,.1},20000},
        PathMediumRay{{-2000,6000,900},{.2,-1,0},20000},PathMediumRay{{0,2200,0},{1,0,0},40000}}) {
        const auto actual=integrate(cloud,ray,1024);
        auto expected=integrate_cloud_reference(cloud_only,*field,{ray.origin,ray.direction,ray.distance_m},reference); REQUIRE(expected);
        CHECK(difference(actual.transmittance,Vec3d::Constant(expected->transmittance))<1e-9);
        const auto full=integrate(mixed,ray,1024),air_value=integrate(snapshot(air_only,s),ray,1024);
        CHECK(difference(full.transmittance,actual.transmittance.cwiseProduct(air_value.transmittance))<2e-6);
        const auto finer=integrate(mixed,ray,2048);
        CHECK(difference(full.transmittance,finer.transmittance)<.005);
    }
}
#endif

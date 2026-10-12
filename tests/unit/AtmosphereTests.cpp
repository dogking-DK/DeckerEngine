#include <dk/render/AtmosphereReference.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <numbers>
using namespace dk;
using namespace dk::render;
namespace {
EnvironmentDescription earth() { EnvironmentDescription e; e.enabled=true; return e; }
EnvironmentDescription vacuum() {
    auto e=earth(); e.atmosphere.rayleigh_scattering_per_m={}; e.atmosphere.mie_scattering_per_m={};
    e.atmosphere.mie_extinction_per_m={}; e.atmosphere.absorption_extinction_per_m={}; e.atmosphere.ground_albedo={}; return e;
}
}
TEST_CASE("reference vacuum and pure absorption have analytic transport") {
    auto e=vacuum(); const Vec3d position{0,1,0};
    const auto empty=integrate_atmosphere_reference(e,position,Vec3d{1,0,0}); REQUIRE(empty);
    CHECK(empty->radiance.isZero()); CHECK(empty->transmittance==Vec3d::Ones());
    e.atmosphere.absorption_extinction_per_m={.00001,.00002,.00003};
    e.atmosphere.absorption_center_height_m=0; e.atmosphere.absorption_half_width_m=e.atmosphere.atmosphere_height_m;
    const auto t=atmosphere_transmittance_reference(e.atmosphere,0,1,512); REQUIRE(t);
    for(int c=0;c<3;++c) CHECK((*t)[c]==Catch::Approx(std::exp(-e.atmosphere.absorption_extinction_per_m[c]*e.atmosphere.atmosphere_height_m/2)).margin(2e-12));
    const auto absorbing=integrate_atmosphere_reference(e,position,Vec3d{0,1,0}); REQUIRE(absorbing); CHECK(absorbing->radiance.isZero());
}
TEST_CASE("reference segments compose and sun energy scales once") {
    auto e=earth(); const Vec3d p{0,1000,0},d=Vec3d{1,.2,0}.normalized();
    AtmosphereReferenceSettings settings{1024,512,10000,false};
    const auto full=integrate_atmosphere_reference(e,p,d,settings); REQUIRE(full);
    settings.max_distance_m=4000; const auto first=integrate_atmosphere_reference(e,p,d,settings); REQUIRE(first);
    settings.max_distance_m=6000; const auto last=integrate_atmosphere_reference(e,p+d*4000,d,settings); REQUIRE(last);
    CHECK((full->radiance-first->radiance-first->transmittance.cwiseProduct(last->radiance)).norm()<1e-7);
    CHECK((full->transmittance-first->transmittance.cwiseProduct(last->transmittance)).norm()<1e-7);
    e.sun.irradiance={3,2,4}; settings.max_distance_m=10000;
    const auto scaled=integrate_atmosphere_reference(e,p,d,settings); REQUIRE(scaled);
    CHECK((scaled->radiance-full->radiance.cwiseProduct(Vec3d{3,2,4})).norm()<1e-10);
}
TEST_CASE("phase functions integrate to one and Mie peaks toward sun") {
    constexpr unsigned steps=100000; double ray=0,mie=0;
    for(unsigned i=0;i<steps;++i) { const double mu=-1+2*(i+.5)/steps; ray+=rayleigh_phase(mu); mie+=mie_phase(mu,.8); }
    CHECK(ray*4*std::numbers::pi/steps==Catch::Approx(1).margin(1e-9));
    CHECK(mie*4*std::numbers::pi/steps==Catch::Approx(1).margin(2e-7));
    CHECK(mie_phase(1,.8)>mie_phase(-1,.8));
}
TEST_CASE("spherical boundaries and planet shadow remain finite") {
    auto e=earth();
    for(double h:{0.,1.,10000.,100000.,150000.}) for(const Vec3d d:{Vec3d{0,1,0},Vec3d{1,0,0},Vec3d{0,-1,0}}) {
        const auto v=integrate_atmosphere_reference(e,Vec3d{0,h,0},d,{128,128}); REQUIRE(v);
        CHECK(v->radiance.allFinite()); CHECK((v->radiance.array()>=0).all());
        CHECK(v->transmittance.allFinite()); CHECK((v->transmittance.array()>=0).all()); CHECK((v->transmittance.array()<=1).all());
        if(h==150000 && d.y()>=0) { CHECK(v->radiance.isZero()); CHECK(v->transmittance==Vec3d::Ones()); }
    }
    const auto ground=integrate_atmosphere_reference(e,Vec3d{0,1,0},Vec3d{0,-1,0}); REQUIRE(ground);
    CHECK(ground->ground); CHECK(ground->distance_m==Catch::Approx(1).margin(1e-8));
    CHECK(atmosphere_transmittance_reference(e.atmosphere,1,-1)->isZero());
    const auto grazing=atmosphere_transmittance_reference(e.atmosphere,100000,-.02); REQUIRE(grazing);
    CHECK((grazing->array()>.99).all()); // Downward ray at the top still misses the planet.
    const auto aboveGround=integrate_atmosphere_reference(e,Vec3d{0,100000,0},Vec3d{1,-.02,0}); REQUIRE(aboveGround);
    CHECK_FALSE(aboveGround->ground); CHECK(aboveGround->distance_m>200000);
    CHECK_FALSE(integrate_atmosphere_reference(e,Vec3d{0,-1,0},Vec3d{0,1,0}));
    CHECK_FALSE(integrate_atmosphere_reference(e,Vec3d::Zero(),Vec3d::Zero()));
    CHECK_FALSE(integrate_atmosphere_reference(e,Vec3d{1e150,0,0},Vec3d{0,1,0}));
    CHECK_FALSE(atmosphere_transmittance_reference(e.atmosphere,1e150,0));
    CHECK_FALSE(integrate_atmosphere_reference(e,Vec3d::Zero(),Vec3d{0,1,0},{0,128}));
}
TEST_CASE("solar disk has physical energy and is planet occluded") {
    auto e=vacuum(); e.sun.angular_radius_rad=.01; AtmosphereReferenceSettings s; s.sun_disk=true;
    const auto disk=integrate_atmosphere_reference(e,Vec3d{0,1,0},Vec3d{0,1,0},s); REQUIRE(disk);
    CHECK(disk->radiance.x()==Catch::Approx(1/(std::numbers::pi*std::pow(std::sin(.01),2))));
    e.sun.direction={0,-1,0}; const auto blocked=integrate_atmosphere_reference(e,Vec3d{0,1,0},Vec3d{0,-1,0},s); REQUIRE(blocked);
    CHECK(blocked->ground); CHECK(blocked->radiance.isZero());
    e.enabled=false; const auto disabled=integrate_atmosphere_reference(e,Vec3d{0,1,0},Vec3d{0,1,0},s); REQUIRE(disabled); CHECK(disabled->radiance.isZero());
}
TEST_CASE("reference convergence and isotropic multiple scattering closure") {
    auto e=earth(); e.sun.direction={1,.05,0}; const Vec3d direction=Vec3d{1,.002,0}.normalized();
    const auto a=integrate_atmosphere_reference(e,Vec3d{0,1,0},direction,{256,256}); REQUIRE(a);
    const auto b=integrate_atmosphere_reference(e,Vec3d{0,1,0},direction,{1024,1024}); REQUIRE(b);
    CHECK((a->radiance-b->radiance).norm()/b->radiance.norm()<.01);
    auto v=vacuum(); const auto zero=multiple_scattering_reference(v.atmosphere,1000,.5,4,32,32); REQUIRE(zero);
    CHECK(zero->radiance.isZero()); CHECK(zero->feedback.isZero());
    const auto ms=multiple_scattering_reference(e.atmosphere,1000,.5,8,64,128); REQUIRE(ms);
    CHECK(ms->radiance.allFinite()); CHECK((ms->radiance.array()>0).all()); CHECK((ms->feedback.array()<1).all());
}

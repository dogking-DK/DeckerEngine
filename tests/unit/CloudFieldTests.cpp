#include <dk/render/CloudFields.hpp>
#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <cmath>
#include <limits>

using namespace dk;
using namespace dk::render;
namespace {
CloudParameters small() { CloudParameters c; c.weather_resolution=16; c.base_resolution=16; c.detail_resolution=8; return c; }
}
TEST_CASE("cloud fields reproduce seed bytes and repeat filtered texel coordinates", "[clouds]") {
    CloudFieldCache first,second; const auto c=small();
    const auto a=first.prepare(c).value(), b=second.prepare(c).value();
    CHECK(a->hash()==0x873bb341ee4ddebdULL); // Generator-v1 byte contract, independently evaluated integer fixture.
    CHECK(a->hash()==b->hash()); CHECK(a->bytes()==512+4096+512);
    for(auto field:{CloudField::weather,CloudField::base,CloudField::detail}) {
        const auto bytes=a->texels(field);
        CHECK(std::ranges::equal(bytes,b->texels(field)));
        CHECK(*std::min_element(bytes.begin(),bytes.end())<64);
        CHECK(*std::max_element(bytes.begin(),bytes.end())>190);
        const unsigned n=field==CloudField::detail ? 8u : 16u, channels=field==CloudField::weather ? 2u : 1u;
        auto center=a->sample(field,{.5/n,.5/n,.5/n}).value();
        for(unsigned k=0;k<channels;++k) CHECK(center[k]==bytes[k]/255.0);
        auto middle=a->sample(field,{1.0/n,1.0/n,1.0/n}).value();
        for(unsigned k=0;k<channels;++k) {
            double sum=0; const unsigned depth=field==CloudField::weather ? 1u : 2u;
            for(unsigned z=0;z<depth;++z) for(unsigned y=0;y<2;++y) for(unsigned x=0;x<2;++x)
                sum+=bytes[((z*n+y)*n+x)*channels+k]/255.0;
            CHECK(std::abs(middle[k]-sum/(4*depth))<1e-14);
        }
        auto seam=a->sample(field,{0,0,0}).value();
        CHECK(seam==a->sample(field,{-1,2,-3}).value());
        auto left=a->sample(field,{-1e-8,.37,.29}).value(),right=a->sample(field,{1e-8,.37,.29}).value();
        CHECK(std::abs(left[0]-right[0])<1e-6);
        CHECK_FALSE(a->sample(field,{std::numeric_limits<double>::quiet_NaN(),0,0}));
    }
    auto other=c; ++other.seed; const auto changed=second.prepare(other).value();
    CHECK(changed->hash()!=a->hash());
}
TEST_CASE("cloud fields bound live generations and bytes before replacing current", "[clouds]") {
    auto c=small(); CloudFieldCache cache;
    auto old=cache.prepare(c).value(); const auto old_hash=old->hash();
    c.coverage=.1; c.density=2; c.wind_velocity_mps={4,7};
    CHECK(cache.prepare(c).value()==old);
    ++c.seed; auto current=cache.prepare(c).value(); CHECK(current->generation()==2);
    ++c.seed; CHECK_FALSE(cache.prepare(c)); CHECK(cache.current()==current); CHECK(old->hash()==old_hash);
    old.reset(); auto next=cache.prepare(c).value(); CHECK(next->generation()==3);
    auto invalid=c; invalid.generator_version=2; CHECK_FALSE(cache.prepare(invalid)); CHECK(cache.current()==next);
    CloudFieldCache limited({6000,2}); auto first=limited.prepare(small()).value();
    CHECK_FALSE(limited.prepare(c)); CHECK(limited.current()==first);
    CloudFieldCache tiny({1,2}); CHECK_FALSE(tiny.prepare(c)); CHECK_FALSE(tiny.current());
    CloudFieldCache invalid_limits({10000,3}); CHECK_FALSE(invalid_limits.prepare(c));
}
TEST_CASE("cloud density respects spherical heights coverage and explicit wind time", "[clouds]") {
    EnvironmentDescription e; e.clouds=small(); e.clouds.enabled=true; e.clouds.coverage=1; e.clouds.density=3;
    CloudFieldCache cache; auto data=cache.prepare(e.clouds).value();
    for(double height:{-1.,0.,1500.,4000.,100001.}) CHECK(sample_cloud_density(e,*data,{0,height,0},0).value()==0);
    double maximum=0;
    for(int i=0;i<512;++i) {
        EnvironmentVector p{double(i*79-20000),2000.+(i%11)*100,double(i*43-10000)};
        double time=i%2 ? -124.25 : 931.;
        const auto density=sample_cloud_density(e,*data,p,time).value(); maximum=std::max(maximum,density);
        CHECK(std::isfinite(density)); CHECK(density>=0); CHECK(density<=e.clouds.density);
        CHECK(sample_cloud_density(e,*data,p,time).value()==density);
    }
    CHECK(maximum>.1);
    for(int i=0;i<32;++i) {
        const EnvironmentVector p{double(i*231-3000),2400,double(i*371-5000)};
        double previous=0;
        for(double coverage:{0.,.1,.5,.8,1.}) {
            e.clouds.coverage=coverage; const double value=sample_cloud_density(e,*data,p,10).value();
            CHECK(value>=previous); previous=value;
        }
    }
    // Equal-radius points and an exact wind offset isolate advection from the spherical profile.
    const auto left=sample_cloud_density(e,*data,{-1000,2300,0},0).value();
    CHECK(std::abs(left-sample_cloud_density(e,*data,{1000,2300,0},200).value())<1e-12);
    CHECK(sample_cloud_density(e,*data,{-1000,2300,0},0).value()==left);
    auto extreme=e; extreme.clouds.wind_velocity_mps={1000,1000};
    extreme.clouds.weather_scale_m=extreme.clouds.shape_scale_m=extreme.clouds.detail_scale_m=1;
    const auto distant_time=sample_cloud_density(extreme,*data,{-100,2300,-100},1e9);
    REQUIRE(distant_time); CHECK(*distant_time>=0); CHECK(*distant_time<=e.clouds.density);
    e.clouds.coverage=0; CHECK(sample_cloud_density(e,*data,{0,2300,0},1e9).value()==0);
    e.clouds.coverage=1; e.clouds.density=0; CHECK(sample_cloud_density(e,*data,{0,2300,0},0).value()==0);
    e.clouds.density=1; e.clouds.enabled=false; CHECK(sample_cloud_density(e,*data,{0,2300,0},0).value()==0);
    CHECK_FALSE(sample_cloud_density(e,*data,{0,2300,0},std::numeric_limits<double>::infinity()));
    CHECK_FALSE(sample_cloud_density(e,*data,{0,2300,0},1e10));
    CHECK_FALSE(sample_cloud_density(e,*data,{1e10,2300,0},0));
    ++e.clouds.seed; CHECK_FALSE(sample_cloud_density(e,*data,{0,2300,0},0));
}

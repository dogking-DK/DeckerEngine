#pragma once
#include <dk/render/Atmosphere.hpp>
#include <algorithm>
#include <numbers>

namespace dk::render::detail {
// The same 256 directions as sunlight.slang's original per-fragment quadrature.
// Cache its smooth cosine convolution over normal direction, not sky radiance.
inline Vector<std::array<float,4>> sky_diffuse_table(memory::ResourceHandle heap,
    const AtmosphereLutView& lut,double radius,double height) {
    constexpr float pi=std::numbers::pi_v<float>;
    struct Sample { Vec3f direction,radiance; }; std::array<Sample,256> samples;
    const double horizon=std::asin(-std::sqrt(height*(2*radius+height))/(radius+height));
    for(unsigned y=0;y<16;++y) for(unsigned x=0;x<16;++x) {
        const double mu=1-2*(y+.5)/16,phi=2*std::numbers::pi*(x+.5)/16-std::numbers::pi;
        const double elevation=std::asin(mu),s=std::sqrt(1-mu*mu);
        const double u=(phi/std::numbers::pi*.5+.5)*191;
        const double v=elevation<horizon ? 53*(1-std::sqrt(std::clamp((horizon-elevation)/(horizon+std::numbers::pi/2),0.,1.))) :
            54+53*std::sqrt(std::clamp((elevation-horizon)/(std::numbers::pi/2-horizon),0.,1.));
        const auto x0=unsigned(u),y0=unsigned(v),x1=std::min(x0+1,lut.width-1),y1=std::min(y0+1,lut.height-1);
        auto rgb=[&](unsigned col,unsigned row) { const auto& p=lut.texels[std::size_t(row)*lut.width+col]; return Vec3f{p[0],p[1],p[2]}; };
        const float fx=float(u-x0),fy=float(v-y0);
        samples[y*16+x]={{float(s*std::cos(phi)),float(mu),float(s*std::sin(phi))},
            ((1-fx)*rgb(x0,y0)+fx*rgb(x1,y0))*(1-fy)+((1-fx)*rgb(x0,y1)+fx*rgb(x1,y1))*fy};
    }
    Vector<std::array<float,4>> result(129*65,memory::Allocator<std::array<float,4>>{heap});
    for(unsigned y=0;y<=64;++y) for(unsigned x=0;x<=128;++x) {
        const float theta=pi*float(y)/64,phi=2*pi*float(x)/128-pi;
        const Vec3f n{std::sin(theta)*std::cos(phi),std::cos(theta),std::sin(theta)*std::sin(phi)};
        Vec3f sum=Vec3f::Zero();
        for(const auto& s:samples) sum+=s.radiance*std::max(0.f,n.dot(s.direction));
        sum*=4*pi/256; result[y*129+x]={sum.x(),sum.y(),sum.z(),1};
    }
    return result;
}
}

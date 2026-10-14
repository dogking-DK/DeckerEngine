#pragma once
#include <dk/math/Types.hpp>
#include <cmath>
#include <numbers>

// Independent scalar oracle: angular GGX D and Smith Lambda, quadrature only.
// It deliberately does not call production BSDF or visible-normal sampling.
namespace path_oracle {
using dk::Vec3d;
constexpr double pi=std::numbers::pi;
inline double distribution(double cosine,double alpha) {
    const double tangent2=(1-cosine*cosine)/(cosine*cosine);
    const double term=alpha*alpha+tangent2;
    return alpha*alpha/(pi*cosine*cosine*cosine*cosine*term*term);
}
inline double lambda(double cosine,double alpha) {
    return (std::sqrt(1+alpha*alpha*(1-cosine*cosine)/(cosine*cosine))-1)/2;
}
inline Vec3d specular(const Vec3d& color,double metal,double rough,const Vec3d& wo,const Vec3d& wi) {
    if(wo.z()<=0 || wi.z()<=0) return Vec3d::Zero();
    const Vec3d h=(wo+wi).normalized(); const double alpha=std::max(.0025,rough*rough);
    Vec3d result;
    for(int k=0;k<3;++k) {
        const double f0=.04*(1-metal)+color[k]*metal,f=f0+(1-f0)*std::pow(1-wo.dot(h),5);
        result[k]=f*distribution(h.z(),alpha)/(4*wo.z()*wi.z()*(1+lambda(wo.z(),alpha)+lambda(wi.z(),alpha)));
    }
    return result;
}
inline Vec3d diffuse(const Vec3d& color,double metal,const Vec3d& wo,const Vec3d& wi) {
    if(wo.z()<=0 || wi.z()<=0) return Vec3d::Zero();
    return color*((1-metal)*.96*.96*(1-std::pow(1-wo.z(),5))*(1-std::pow(1-wi.z(),5))/pi);
}
inline Vec3d evaluate(const Vec3d& color,double metal,double rough,const Vec3d& wo,const Vec3d& wi) {
    return specular(color,metal,rough,wo,wi)+diffuse(color,metal,wo,wi);
}
inline Vec3d furnace(const Vec3d& color,double metal,double rough,const Vec3d& wo,int size) {
    Vec3d sum=Vec3d::Zero(); const double a=std::max(.0025,rough*rough);
    for(int i=0;i<size;++i) for(int j=0;j<2*size;++j) {
        const double u=(i+.5)/size,phi=2*pi*(j+.5)/(2*size);
        // Full NDF integration, not the production visible-normal distribution.
        const double tan2=a*a*u/(1-u),ch=1/std::sqrt(1+tan2),sh=std::sqrt(1-ch*ch);
        const Vec3d h{sh*std::cos(phi),sh*std::sin(phi),ch},wi=2*wo.dot(h)*h-wo;
        if(wi.z()>0 && wo.dot(h)>0) sum+=specular(color,metal,rough,wo,wi)*(wi.z()*4*wo.dot(h)/(distribution(ch,a)*ch));
        const double z=u,r=std::sqrt(1-z*z); const Vec3d diffuse_wi{r*std::cos(phi),r*std::sin(phi),z};
        sum+=diffuse(color,metal,wo,diffuse_wi)*(z*2*pi);
    }
    return sum/(2.0*size*size);
}
inline Vec3d area(const Vec3d& color,double metal,double rough,const Vec3d& wo,int size) {
    Vec3d sum=Vec3d::Zero();
    for(int i=0;i<size;++i) for(int j=0;j<size;++j) {
        const Vec3d point{-1+2*(i+.5)/size,-1+2*(j+.5)/size,2}; const double d2=point.squaredNorm(); const Vec3d wi=point.normalized();
        sum+=evaluate(color,metal,rough,wo,wi)*(wi.z()*wi.z()/d2);
    }
    return sum*(4.0/(size*size));
}
}

#include "PathBsdf.hpp"
#include <algorithm>
#include <cmath>
#include <numbers>

namespace dk::render::detail {
namespace {
constexpr double pi=std::numbers::pi;
double alpha(const PathBsdf& b) { return std::max(.0025,b.roughness*b.roughness); }
double fifth(double x) { const double x2=x*x; return x2*x2*x; }
struct Frame {
    Vec3d x,y,z;
    explicit Frame(const Vec3d& n):z(n) { x=(std::abs(n.z())<.9 ? Vec3d::UnitZ() : Vec3d::UnitY()).cross(n).normalized(); y=n.cross(x); }
    Vec3d local(const Vec3d& v) const { return {x.dot(v),y.dot(v),z.dot(v)}; }
    Vec3d world(const Vec3d& v) const { return (x*v.x()+y*v.y()+z*v.z()).normalized(); }
};
}
PathBsdfEval eval_path_bsdf(const PathBsdf& b,const Vec3d& n,const Vec3d& wo,const Vec3d& wi) {
    const double no=n.dot(wo),ni=n.dot(wi);
    if(no<=0 || ni<=0) return {};
    if(b.model==PathTraceMaterialModel::lambert) return {b.base_color/pi,ni/pi};
    const Vec3d h=(wo+wi).normalized();
    const double nh=std::clamp(n.dot(h),0.0,1.0),vh=std::clamp(wo.dot(h),0.0,1.0),a=alpha(b),a2=a*a;
    // Cross product avoids subtracting near-equal numbers at a narrow normal peak.
    const double denominator=n.cross(h).squaredNorm()+a2*nh*nh;
    const double d=a2/(pi*denominator*denominator);
    const double so=std::sqrt(a2+(1-a2)*no*no),si=std::sqrt(a2+(1-a2)*ni*ni);
    const double f5=fifth(1-vh),transmission=.96*.96*(1-fifth(1-no))*(1-fifth(1-ni)),q=(1+b.metallic)*.5;
    const Vec3d f0=Vec3d::Constant(.04)*(1-b.metallic)+b.base_color*b.metallic;
    const Vec3d fresnel=f0+(Vec3d::Ones()-f0)*f5;
    return {b.base_color*((1-b.metallic)*transmission/pi)+fresnel*(d/(2*(ni*so+no*si))),
        (1-q)*ni/pi+q*d/(2*(no+so))};
}
PathBsdfSample sample_path_bsdf(const PathBsdf& b,const Vec3d& n,const Vec3d& wo,double u,double v) {
    if(n.dot(wo)<=0 || !(u>=0 && u<1 && v>=0 && v<1)) return {};
    const Frame frame{n}; Vec3d wi;
    const double q=b.model==PathTraceMaterialModel::lambert ? 0 : (1+b.metallic)*.5;
    if(u>=q) {
        const double r=std::sqrt((u-q)/(1-q)),phi=2*pi*v;
        wi=frame.world({r*std::cos(phi),r*std::sin(phi),std::sqrt(std::max(0.0,1-r*r))});
    } else {
        const double a=alpha(b); const Vec3d o=frame.local(wo);
        const Vec3d stretched=Vec3d{a*o.x(),a*o.y(),o.z()}.normalized();
        const double lensq=stretched.x()*stretched.x()+stretched.y()*stretched.y();
        const Vec3d t1=lensq>0 ? Vec3d{-stretched.y(),stretched.x(),0}/std::sqrt(lensq) : Vec3d{1,0,0};
        const Vec3d t2=stretched.cross(t1);
        const double r=std::sqrt(u/q),phi=2*pi*v,x=r*std::cos(phi),s=(1+stretched.z())*.5;
        const double y=(1-s)*std::sqrt(std::max(0.0,1-x*x))+s*r*std::sin(phi);
        const Vec3d projected=t1*x+t2*y+stretched*std::sqrt(std::max(0.0,1-x*x-y*y));
        const Vec3d h=frame.world({a*projected.x(),a*projected.y(),std::max(0.0,projected.z())});
        wi=(2*wo.dot(h)*h-wo).normalized();
        if(n.dot(wi)<=0) return {}; // Keep rejected mass; resampling would bias the PDF.
    }
    return {wi,eval_path_bsdf(b,n,wo,wi)};
}
}

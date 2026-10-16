#pragma once
#include <dk/render/PathMediumReference.hpp>
#include <Eigen/Geometry>
#include <algorithm>
#include <cmath>
#include <numbers>

namespace dk::render::detail {
inline double volume_hg(double cosine,double g) {
    return (1-g*g)/(4*std::numbers::pi*std::pow(1+g*g-2*g*cosine,1.5));
}
inline Vec3d volume_direction(const Vec3d& axis,double cosine,double azimuth) {
    const Vec3d u=axis.unitOrthogonal(),v=axis.cross(u);
    const double r=std::sqrt(std::max(0.,1-cosine*cosine)),phi=2*std::numbers::pi*azimuth;
    return (axis*cosine+r*(u*std::cos(phi)+v*std::sin(phi))).normalized();
}
struct VolumePhase {
    Vec3d weights=Vec3d::Zero(); // Rayleigh, Mie, cloud, normalized at the chosen channel.
    double mie_g=0,cloud_g=0;
    [[nodiscard]] double eval(double cosine) const {
        cosine=std::clamp(cosine,-1.,1.);
        return weights.x()*3*(1+cosine*cosine)/(16*std::numbers::pi)+
            weights.y()*volume_hg(cosine,mie_g)+weights.z()*volume_hg(cosine,cloud_g);
    }
    [[nodiscard]] Vec3d sample(const Vec3d& direction,double choose,double u,double v) const {
        double cosine;
        if(choose<weights.x()) cosine=2*std::sinh(std::asinh(4*u-2)/3);
        else {
            const double g=choose<weights.x()+weights.y() ? mie_g : cloud_g;
            if(std::abs(g)<.001) {
                // Algebraically identical inverse CDF, including its continuous g=0 limit.
                const double w=2*u-1;
                cosine=(2*w+g*(w*w+3)+2*g*g*w+g*g*g*(w*w-1))/(2*(1+g*w)*(1+g*w));
            }
            else { const double q=(1-g*g)/(1-g+2*g*u); cosine=(1+g*g-q*q)/(2*g); }
        }
        return volume_direction(direction,std::clamp(cosine,-1.,1.),v);
    }
};
} // namespace dk::render::detail

#include <dk/render/AtmosphereReference.hpp>
#include <algorithm>
#include <cmath>
#include <numbers>

namespace dk::render {
namespace {
constexpr double pi=std::numbers::pi;
Vec3d vector(const EnvironmentVector& v) { return {v[0],v[1],v[2]}; }
struct Medium { Vec3d ray, mie, extinction; };
Medium medium(const AtmosphereParameters& a,double height) {
    const double h=std::max(0.0,height);
    const Vec3d ray=vector(a.rayleigh_scattering_per_m)*std::exp(-h/a.rayleigh_scale_height_m);
    const Vec3d mie=vector(a.mie_scattering_per_m)*std::exp(-h/a.mie_scale_height_m);
    const Vec3d extinction=ray+vector(a.mie_extinction_per_m)*std::exp(-h/a.mie_scale_height_m)+
        vector(a.absorption_extinction_per_m)*std::max(0.0,1-std::abs(h-a.absorption_center_height_m)/a.absorption_half_width_m);
    return {ray,mie,extinction};
}
struct Interval { double begin=0,end=0; bool ground=false; };
std::pair<double,double> roots(double radius,double h,double mu,double boundary_h) {
    const double b=(radius+h)*mu, c=(h-boundary_h)*(2*radius+h+boundary_h), disc=b*b-c;
    if(disc<0) return {1,-1};
    const double q=-b-std::copysign(std::sqrt(disc),b);
    if(q==0) return {0,0};
    const double other=c/q;
    return {std::min(q,other),std::max(q,other)};
}
Interval interval(const AtmosphereParameters& a,double h,double mu) {
    const auto top=roots(a.ground_radius_m,h,mu,a.atmosphere_height_m);
    if(top.second<0 || top.first>top.second) return {};
    Interval result{std::max(0.0,top.first),top.second,false};
    const auto ground=roots(a.ground_radius_m,h,mu,0);
    if(mu<0 && ground.first<=ground.second && ground.first>=0 && ground.first<=result.end) { result.end=ground.first; result.ground=true; }
    return result;
}
double height_at(double radius,double h,double mu,double t) {
    const double r=radius+h, delta=t*(t+2*r*mu);
    return h+delta/(std::sqrt(std::max(0.0,r*r+delta))+r);
}
double node(double i,double count) { return 0.5-0.5*std::cos(pi*i/count); }
Vec3d transmittance(const AtmosphereParameters& a,double h,double mu,std::uint32_t steps) {
    const auto path=interval(a,h,mu);
    if(path.ground) return Vec3d::Zero();
    Vec3d optical=Vec3d::Zero();
    for(std::uint32_t i=0;i<steps;++i) {
        const double begin=std::lerp(path.begin,path.end,node(i,steps)),end=std::lerp(path.begin,path.end,node(i+1,steps));
        optical+=medium(a,height_at(a.ground_radius_m,h,mu,(begin+end)*0.5)).extinction*(end-begin);
    }
    return (-optical.array()).exp().matrix();
}
Vec3d integral_weight(const Vec3d& extinction,double distance) {
    Vec3d result;
    for(int c=0;c<3;++c) result[c]=extinction[c]>0 ? -std::expm1(-extinction[c]*distance)/extinction[c] : distance;
    return result;
}
bool bounded(std::uint32_t steps) { return steps>0 && steps<=8192; }
Result<void> parameters(const AtmosphereParameters& a) {
    EnvironmentDescription e; e.atmosphere=a; const auto valid=normalize_environment(e);
    if(!valid) return std::unexpected(valid.error());
    return {};
}
Error invalid() { return {ErrorCode::invalid_argument,"invalid atmosphere reference ray, height or integration budget"}; }
}
double rayleigh_phase(double c) noexcept { return 3*(1+c*c)/(16*pi); }
double mie_phase(double c,double g) noexcept { return (1-g*g)/(4*pi*std::pow(1+g*g-2*g*c,1.5)); }
Result<Vec3d> atmosphere_transmittance_reference(const AtmosphereParameters& a,double h,double mu,std::uint32_t steps) {
    if(auto valid=parameters(a);!valid) return std::unexpected(valid.error());
    if(!std::isfinite(h) || h<0 || h>4e9 || !std::isfinite(mu) || std::abs(mu)>1 || !bounded(steps)) return std::unexpected(invalid());
    return transmittance(a,h,mu,steps);
}
Result<AtmosphereIntegral> integrate_atmosphere_reference(const EnvironmentDescription& input,const Vec3d& position,
    const Vec3d& direction,const AtmosphereReferenceSettings& settings) {
    auto normalized=normalize_environment(input); if(!normalized) return std::unexpected(normalized.error());
    const auto& e=*normalized; const auto& a=e.atmosphere;
    const double direction_length=direction.stableNorm();
    if(!position.allFinite() || !direction.allFinite() || !std::isfinite(direction_length) || direction_length==0 ||
        !bounded(settings.steps) || !bounded(settings.sun_steps) || !(settings.max_distance_m>=0) ||
        (settings.sun_disk && std::isfinite(settings.max_distance_m))) return std::unexpected(invalid());
    const auto height=atmosphere_height_m(a,{position.x(),position.y(),position.z()});
    if(!height || *height<0) return std::unexpected(invalid());
    const Vec3d delta=position-vector(a.planet_center_m);
    const double h=*height,radius=a.ground_radius_m+h;
    const Vec3d up=delta/radius, ray=direction/direction_length, sun=vector(e.sun.direction);
    const double mu=std::clamp(up.dot(ray),-1.0,1.0), sun_mu=up.dot(sun), cosine=std::clamp(ray.dot(sun),-1.0,1.0);
    auto path=interval(a,h,mu); AtmosphereIntegral result;
    result.ground=path.ground && path.end<=settings.max_distance_m;
    path.end=std::min(path.end,settings.max_distance_m);
    if(path.end>path.begin && e.enabled) {
        result.distance_m=path.end-path.begin;
        for(std::uint32_t i=0;i<settings.steps;++i) {
            const double begin=std::lerp(path.begin,path.end,node(i,settings.steps)),end=std::lerp(path.begin,path.end,node(i+1,settings.steps)),t=(begin+end)*0.5;
            const double sample_h=height_at(a.ground_radius_m,h,mu,t);
            const auto m=medium(a,sample_h);
            const double sample_sun_mu=std::clamp((radius*sun_mu+t*cosine)/(a.ground_radius_m+sample_h),-1.0,1.0);
            const Vec3d source=(m.ray*rayleigh_phase(cosine)+m.mie*mie_phase(cosine,a.mie_anisotropy)).cwiseProduct(
                transmittance(a,std::max(0.0,sample_h),sample_sun_mu,settings.sun_steps)).cwiseProduct(vector(e.sun.irradiance));
            result.radiance+=result.transmittance.cwiseProduct(source).cwiseProduct(integral_weight(m.extinction,end-begin));
            result.transmittance=result.transmittance.cwiseProduct((-(end-begin)*m.extinction.array()).exp().matrix());
        }
    }
    if(settings.sun_disk && e.enabled && !result.ground && cosine>=std::cos(e.sun.angular_radius_rad))
        result.radiance+=result.transmittance.cwiseProduct(vector(*solar_disk_radiance(e.sun)));
    return result;
}
Result<MultipleScatteringReference> multiple_scattering_reference(const AtmosphereParameters& a,double h,double sun_mu,
    std::uint32_t side,std::uint32_t steps,std::uint32_t sun_steps) {
    if(auto valid=parameters(a);!valid) return std::unexpected(valid.error());
    if(!std::isfinite(h) || h<0 || h>a.atmosphere_height_m || !std::isfinite(sun_mu) || std::abs(sun_mu)>1 ||
        side<2 || side>64 || !bounded(steps) || !bounded(sun_steps)) return std::unexpected(invalid());
    MultipleScatteringReference result;
    for(std::uint32_t y=0;y<side;++y) for(std::uint32_t x=0;x<side;++x) {
        const double mu=2*(y+0.5)/side-1,phi=2*pi*(x+0.5)/side;
        const double cosine=mu*sun_mu+std::sqrt((1-mu*mu)*(1-sun_mu*sun_mu))*std::cos(phi);
        const auto path=interval(a,h,mu); Vec3d throughput=Vec3d::Ones();
        for(std::uint32_t i=0;i<steps;++i) {
            const double begin=path.end*node(i,steps),end=path.end*node(i+1,steps),t=(begin+end)*0.5;
            const double sample_h=height_at(a.ground_radius_m,h,mu,t);
            const auto m=medium(a,sample_h); const Vec3d weight=throughput.cwiseProduct(integral_weight(m.extinction,end-begin));
            const double sample_sun_mu=std::clamp(((a.ground_radius_m+h)*sun_mu+t*cosine)/(a.ground_radius_m+sample_h),-1.0,1.0);
            const Vec3d scatter=m.ray+m.mie;
            result.radiance+=weight.cwiseProduct(scatter).cwiseProduct(transmittance(a,std::max(0.0,sample_h),sample_sun_mu,sun_steps))/(4*pi);
            result.feedback+=weight.cwiseProduct(scatter);
            throughput=throughput.cwiseProduct((-(end-begin)*m.extinction.array()).exp().matrix());
        }
        if(path.ground) {
            const double ground_mu=std::clamp(((a.ground_radius_m+h)*sun_mu+path.end*cosine)/a.ground_radius_m,-1.0,1.0);
            result.radiance+=throughput.cwiseProduct(vector(a.ground_albedo)).cwiseProduct(transmittance(a,0,ground_mu,sun_steps))*std::max(0.0,ground_mu)/pi;
        }
    }
    result.radiance/=side*side; result.feedback/=side*side;
    if((result.feedback.array()>=1).any()) return std::unexpected(Error{ErrorCode::not_supported,"multiple scattering closure does not converge"});
    result.radiance=result.radiance.cwiseQuotient(Vec3d::Ones()-result.feedback);
    return result;
}
} // namespace dk::render

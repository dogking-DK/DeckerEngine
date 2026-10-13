#include "CloudReferenceInternal.hpp"
#include <algorithm>
#include <cmath>
#include <numbers>

namespace dk::render {
namespace {
Error invalid(const char* text) { return {ErrorCode::invalid_argument,text}; }
Vec3d vector(const EnvironmentVector& v) { return {v[0],v[1],v[2]}; }
bool range(double x,double a,double b) { return std::isfinite(x) && x>=a && x<=b; }
std::pair<double,double> roots(double radius,double h,double mu,double boundary) {
    const double b=(radius+h)*mu, c=(h-boundary)*(2*radius+h+boundary), disc=b*b-c;
    if(disc<=0) return {1,-1}; // A tangent touches no positive measure volume.
    const double q=-b-std::copysign(std::sqrt(disc),b), other=c/q;
    return {std::min(q,other),std::max(q,other)};
}
CloudIntervals intervals(const EnvironmentDescription& e,double h,double mu,double limit) {
    CloudIntervals result;
    const double r=e.atmosphere.ground_radius_m;
    const auto ground=roots(r,h,mu,0);
    if(mu<0 && ground.first>=0 && ground.first<=ground.second && ground.first<=limit) {
        limit=ground.first; result.ground=true;
    }
    const auto outer=roots(r,h,mu,e.clouds.top_height_m);
    const double begin=std::max(0.,outer.first), end=std::min(limit,outer.second);
    if(end<=begin) return result;
    const auto inner=roots(r,h,mu,e.clouds.bottom_height_m);
    const double cut_begin=std::max(begin,inner.first),cut_end=std::min(end,inner.second);
    if(cut_end<=cut_begin) { result.segments[0]={begin,end}; result.count=1; }
    else {
        if(cut_begin>begin) result.segments[result.count++]={begin,cut_begin};
        if(end>cut_end) result.segments[result.count++]={cut_end,end};
    }
    return result;
}
double height_at(double r,double h,double mu,double t) {
    const double radius=r+h, delta=t*(t+2*radius*mu);
    return h+delta/(std::sqrt(std::max(0.,radius*radius+delta))+radius);
}
double density(const EnvironmentDescription& e,const CloudFieldData& data,const CloudReferenceSettings& s,const Vec3d& p,double h) {
    if(s.homogeneous) return e.clouds.density*s.homogeneous_density;
    return detail::cloud_density_at(e,data,{p.x(),p.y(),p.z()},s.time_seconds,h);
}
double sun_optical(const EnvironmentDescription& e,const CloudFieldData& data,const CloudReferenceSettings& s,
    const Vec3d& p,double h,double mu) {
    const auto path=intervals(e,h,mu,2*(e.atmosphere.ground_radius_m+e.clouds.top_height_m));
    if(path.ground) return -1; // Planet shadow affects every order, including the no-self-shadow diagnostic.
    if(!s.self_shadow) return 0;
    double optical=0; const auto sun=vector(e.sun.direction);
    for(unsigned k=0;k<path.count;++k) {
        const auto segment=path.segments[k]; const double ds=(segment.end_m-segment.begin_m)/s.sun_steps;
        for(unsigned i=0;i<s.sun_steps;++i) {
            const double t=segment.begin_m+(i+.5)*ds, sh=height_at(e.atmosphere.ground_radius_m,h,mu,t);
            optical+=density(e,data,s,p+sun*t,sh)*e.clouds.extinction_per_m*ds;
        }
    }
    return optical;
}
}
namespace detail {
Result<EnvironmentDescription> validate_cloud_reference(const EnvironmentDescription& input,const CloudReferenceSettings& s,std::size_t rays) {
    auto normalized=normalize_environment(input); if(!normalized) return std::unexpected(normalized.error());
    const auto& a=normalized->atmosphere;
    if(a.ground_radius_m>1e7 || a.atmosphere_height_m>1e6 ||
        normalized->clouds.top_height_m-normalized->clouds.bottom_height_m<1 ||
        !std::ranges::all_of(a.planet_center_m,[](double v){ return std::abs(v)<=1e8; }))
        return std::unexpected(Error{ErrorCode::not_supported,"Cloud reference geometry outside supported domain"});
    if(!rays || rays>4096 || s.model_version!=1 || !s.steps || s.steps>4096 || !s.sun_steps || s.sun_steps>512 ||
        !s.atmosphere_steps || s.atmosphere_steps>512 || !range(s.time_seconds,-1e9,1e9) ||
        !range(s.phase_g,-.95,.95) || !range(s.single_scattering_albedo,0,1) || !range(s.multiple_scattering,0,.5) ||
        !s.sky_radiance.allFinite() || (s.sky_radiance.array()<0).any() || (s.sky_radiance.array()>1e6).any() ||
        !range(s.homogeneous_density,0,1) || !s.max_work || s.max_work>256*1024*1024 || !s.max_bytes || s.max_bytes>4*1024*1024)
        return std::unexpected(invalid("Invalid cloud reference model, ray count or settings"));
    const auto work=std::uint64_t{rays}*2*s.steps*(2*s.sun_steps+s.atmosphere_steps+1);
    if(work>s.max_work) return std::unexpected(Error{ErrorCode::invalid_state,"Cloud reference work budget exceeded"});
    return normalized;
}
Result<CloudPreparedRay> prepare_cloud_ray(const EnvironmentDescription& e,const CloudRay& input) {
    if(!input.origin_m.allFinite() || (input.origin_m.array().abs()>1e9).any() || !input.direction.allFinite() ||
        !range(input.max_distance_m,0,2e7)) return std::unexpected(invalid("Invalid cloud reference ray"));
    const double scale=input.direction.cwiseAbs().maxCoeff();
    if(scale==0) return std::unexpected(invalid("Cloud ray direction is zero"));
    CloudPreparedRay p; p.ray=input; p.ray.direction=(input.direction/scale).normalized();
    if(((p.ray.origin_m+p.ray.direction*p.ray.max_distance_m).array().abs()>1e9).any())
        return std::unexpected(invalid("Cloud ray endpoint exceeds Scene position bounds"));
    const Vec3d delta=input.origin_m-vector(e.atmosphere.planet_center_m); const double radius=delta.norm();
    p.height=radius-e.atmosphere.ground_radius_m;
    if(p.height<0) return std::unexpected(invalid("Cloud ray starts below ground"));
    const Vec3d up=delta/radius, sun=vector(e.sun.direction);
    p.mu=std::clamp(up.dot(p.ray.direction),-1.,1.); p.sun_mu=std::clamp(up.dot(sun),-1.,1.);
    p.cosine=std::clamp(p.ray.direction.dot(sun),-1.,1.);
    p.intervals=intervals(e,p.height,p.mu,input.max_distance_m); return p;
}
}
Result<CloudIntervals> cloud_shell_intervals(const EnvironmentDescription& input,const CloudRay& ray) {
    auto e=detail::validate_cloud_reference(input,{},1); if(!e) return std::unexpected(e.error());
    auto prepared=detail::prepare_cloud_ray(*e,ray); if(!prepared) return std::unexpected(prepared.error());
    return prepared->intervals;
}
Result<CloudIntegral> integrate_cloud_reference(const EnvironmentDescription& input,const CloudFieldData& data,
    const CloudRay& ray,const CloudReferenceSettings& s) {
    const auto env=detail::validate_cloud_reference(input,s,1); if(!env) return std::unexpected(env.error());
    const auto& e=*env; const auto& c=e.clouds; const auto& a=e.atmosphere;
    if(data.key()!=cloud_field_key(c) || !data.bytes()) return std::unexpected(invalid("Cloud reference field key mismatch"));
    const auto prepared=detail::prepare_cloud_ray(e,ray); if(!prepared) return std::unexpected(prepared.error());
    const auto& p=*prepared; CloudIntegral result;
    if(!c.enabled || c.coverage==0 || c.density==0 || c.extinction_per_m==0) return result;
    const double direct_phase=mie_phase(p.cosine,s.phase_g), second_phase=mie_phase(p.cosine,s.phase_g*.5), third_phase=mie_phase(p.cosine,s.phase_g*.25);
    double opacity=0, moment=0;
    for(unsigned k=0;k<p.intervals.count;++k) {
        const auto segment=p.intervals.segments[k]; const double ds=(segment.end_m-segment.begin_m)/s.steps;
        result.distance_m+=segment.end_m-segment.begin_m;
        for(unsigned i=0;i<s.steps;++i) {
            const double t=segment.begin_m+(i+.5)*ds, h=height_at(a.ground_radius_m,p.height,p.mu,t);
            const Vec3d position=p.ray.origin_m+p.ray.direction*t;
            const double sigma=density(e,data,s,position,h)*c.extinction_per_m;
            const double sample_sun_mu=std::clamp(((a.ground_radius_m+p.height)*p.sun_mu+t*p.cosine)/(a.ground_radius_m+h),-1.,1.);
            const double tau=sun_optical(e,data,s,position,h,sample_sun_mu);
            Vec3d source=s.sky_radiance;
            if(tau>=0) {
                Vec3d air=Vec3d::Ones();
                if(e.enabled) {
                    auto value=atmosphere_transmittance_reference(a,std::max(0.,h),sample_sun_mu,s.atmosphere_steps);
                    if(!value) return std::unexpected(value.error()); air=*value;
                }
                const double phase=(1-s.multiple_scattering)*direct_phase*std::exp(-tau)+s.multiple_scattering*(
                    2./3*second_phase*std::exp(-tau*.5)+1./3*third_phase*std::exp(-tau*.25));
                source+=vector(e.sun.irradiance).cwiseProduct(air)*phase;
            }
            const double weight=result.transmittance*-std::expm1(-sigma*ds);
            result.radiance+=weight*s.single_scattering_albedo*source;
            result.transmittance*=std::exp(-sigma*ds); opacity+=weight; moment+=weight*t; ++result.samples;
        }
    }
    if(opacity>0) result.mean_distance_m=moment/opacity;
    return result;
}
Result<void> validate_cloud_composition(const EnvironmentDescription& e,const CloudCompositionSettings& s,std::size_t pixels) {
    auto valid=detail::validate_cloud_reference(e,s.reference,1); if(!valid) return std::unexpected(valid.error());
    if(!pixels || pixels>4096 || !s.max_work || s.max_work>1024ULL*1024*1024)
        return std::unexpected(invalid("Cloud composition requires 1..4096 pixels and a bounded work limit"));
    const auto& r=s.reference;
    if(std::uint64_t{pixels}*5*r.steps*(2*r.sun_steps+r.atmosphere_steps+1)>s.max_work)
        return std::unexpected(Error{ErrorCode::invalid_state,"Cloud composition work budget exceeded"});
    if(e.atmosphere.ground_radius_m<1000 || e.clouds.weather_scale_m<100 || e.clouds.shape_scale_m<100 || e.clouds.detail_scale_m<10)
        return std::unexpected(Error{ErrorCode::not_supported,"Cloud composition outside GPU geometry/noise domain"});
    return {};
}
Result<CloudMediumIntegral> integrate_cloud_medium_reference(const EnvironmentDescription& input,const CloudFieldData& data,
    const CloudRay& ray,const CloudReferenceSettings& s) {
    auto env=detail::validate_cloud_reference(input,s,1); if(!env) return std::unexpected(env.error());
    if(std::uint64_t{5}*s.steps*(2*s.sun_steps+s.atmosphere_steps+1)>s.max_work)
        return std::unexpected(Error{ErrorCode::invalid_state,"Cloud medium reference work budget exceeded"});
    const auto& e=*env; const auto& a=e.atmosphere; const auto& c=e.clouds;
    if(data.key()!=cloud_field_key(c) || !data.bytes()) return std::unexpected(invalid("Cloud medium field key mismatch"));
    auto prepared=detail::prepare_cloud_ray(e,ray); if(!prepared) return std::unexpected(prepared.error());
    const auto& p=*prepared; CloudMediumIntegral result;
    const auto air=roots(a.ground_radius_m,p.height,p.mu,a.atmosphere_height_m);
    const auto ground=roots(a.ground_radius_m,p.height,p.mu,0);
    const double begin=std::max(0.,air.first); double end=std::min(p.ray.max_distance_m,air.second);
    if(p.mu<0 && ground.first>=0 && ground.first<=ground.second && ground.first<=p.ray.max_distance_m) {
        end=std::min(end,ground.first); result.ground=true;
    }
    if(end<=begin) return result;
    const bool cloud=c.enabled && c.coverage>0 && c.density>0 && c.extinction_per_m>0;
    std::array<double,6> cuts{}; unsigned n=0; cuts[n++]=begin;
    for(unsigned k=0;k<p.intervals.count;++k) {
        const auto interval=p.intervals.segments[k];
        cuts[n++]=std::clamp(interval.begin_m,begin,end); cuts[n++]=std::clamp(interval.end_m,begin,end);
    }
    cuts[n++]=end;
    double opacity=0,moment=0;
    for(unsigned k=1;k<n;++k) {
        const double first=cuts[k-1],last=cuts[k]; if(last<=first) continue;
        const double middle=(first+last)*.5;
        bool in_cloud=false;
        for(unsigned j=0;j<p.intervals.count;++j) in_cloud|=middle>=p.intervals.segments[j].begin_m && middle<p.intervals.segments[j].end_m;
        for(unsigned i=0;i<s.steps;++i) {
            auto node=[&](unsigned index) { const double fraction=double(index)/s.steps; return in_cloud ? fraction : .5-.5*std::cos(std::numbers::pi*fraction); };
            const double start=std::lerp(first,last,node(i)),finish=std::lerp(first,last,node(i+1)),ds=finish-start,t=(start+finish)*.5;
            const double h=height_at(a.ground_radius_m,p.height,p.mu,t);
            const Vec3d position=p.ray.origin_m+p.ray.direction*t;
            const double sun_mu=std::clamp(((a.ground_radius_m+p.height)*p.sun_mu+t*p.cosine)/(a.ground_radius_m+h),-1.,1.);
            Vec3d extinction=Vec3d::Zero(),air_source=Vec3d::Zero(),cloud_source=Vec3d::Zero(),sun_air=Vec3d::Ones();
            if(e.enabled) {
                auto trans=atmosphere_transmittance_reference(a,std::max(0.,h),sun_mu,s.atmosphere_steps);
                if(!trans) return std::unexpected(trans.error()); sun_air=*trans;
                const Vec3d rayleigh=vector(a.rayleigh_scattering_per_m)*std::exp(-h/a.rayleigh_scale_height_m);
                const Vec3d mie=vector(a.mie_scattering_per_m)*std::exp(-h/a.mie_scale_height_m);
                extinction=rayleigh+vector(a.mie_extinction_per_m)*std::exp(-h/a.mie_scale_height_m)+
                    vector(a.absorption_extinction_per_m)*std::max(0.,1-std::abs(h-a.absorption_center_height_m)/a.absorption_half_width_m);
                air_source=(rayleigh*rayleigh_phase(p.cosine)+mie*mie_phase(p.cosine,a.mie_anisotropy)).cwiseProduct(sun_air).cwiseProduct(vector(e.sun.irradiance));
            }
            const double sigma=cloud && in_cloud ? density(e,data,s,position,h)*c.extinction_per_m : 0;
            if(cloud && in_cloud) {
                const double tau=sun_optical(e,data,s,position,h,sun_mu);
                Vec3d source=s.sky_radiance;
                if(tau>=0) {
                    const double phase=(1-s.multiple_scattering)*mie_phase(p.cosine,s.phase_g)*std::exp(-tau)+s.multiple_scattering*(
                        2./3*mie_phase(p.cosine,s.phase_g*.5)*std::exp(-tau*.5)+1./3*mie_phase(p.cosine,s.phase_g*.25)*std::exp(-tau*.25));
                    source+=vector(e.sun.irradiance).cwiseProduct(sun_air)*phase;
                }
                cloud_source=sigma*s.single_scattering_albedo*source;
            }
            extinction.array()+=sigma; Vec3d weight;
            for(int channel=0;channel<3;++channel) weight[channel]=extinction[channel]>0 ? -std::expm1(-extinction[channel]*ds)/extinction[channel] : ds;
            weight=weight.cwiseProduct(result.transmittance);
            result.radiance+=weight.cwiseProduct(air_source+cloud_source); result.cloud_radiance+=weight.cwiseProduct(cloud_source);
            result.transmittance=result.transmittance.cwiseProduct((-extinction.array()*ds).exp().matrix());
            const double cloud_weight=result.cloud_transmittance*-std::expm1(-sigma*ds);
            result.cloud_transmittance*=std::exp(-sigma*ds); opacity+=cloud_weight; moment+=cloud_weight*t;
        }
    }
    if(opacity>0) result.mean_cloud_distance_m=moment/opacity;
    return result;
}
Result<void> validate_cloud_optimization(const EnvironmentDescription& e,const CloudOptimizedSettings& s,std::uint32_t width,std::uint32_t height) {
    auto valid=validate_cloud_composition(e,s.composition,1); if(!valid) return valid;
    const auto& r=s.composition.reference;
    if(!width || !height || width>4096 || height>4096 || std::uint64_t{width}*height>1920*1080 || !s.view_id ||
        r.steps>256 || r.sun_steps>64 || !range(s.termination_transmittance,0,1e-4) || !range(s.history_weight,0,.9) ||
        !s.max_work || s.max_work>64ULL*1024*1024*1024)
        return std::unexpected(invalid("Invalid cloud optimization extent, identity, sampling or reconstruction settings"));
    const auto pixels=std::uint64_t{width}*height+std::uint64_t{(width+1)/2}*((height+1)/2);
    if(pixels*256*(5+2*(2*std::max(r.sun_steps,16u)+4))>s.max_work)
        return std::unexpected(Error{ErrorCode::invalid_state,"Cloud optimization work budget exceeded"});
    return {};
}
} // namespace dk::render

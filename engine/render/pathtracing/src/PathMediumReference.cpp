#include <dk/render/PathMediumReference.hpp>
#include <Eigen/Geometry>
#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

namespace dk::render {
namespace {
Error invalid(const char* message) { return {ErrorCode::invalid_argument,message}; }
Error cancelled() { return {ErrorCode::invalid_state,"Path medium integration cancelled",{"render.pathtracing.medium.cancelled"}}; }
Vec3d vector(const EnvironmentVector& v) { return {v[0],v[1],v[2]}; }
bool bounded(double x,double low,double high) { return std::isfinite(x) && x>=low && x<=high; }
bool position_valid(const Vec3d& p) { return p.allFinite() && p.cwiseAbs().maxCoeff()<=1e9; }
struct PreparedRay { Vec3d direction; double height=0,b=0,perpendicular=0; };
// Difference of squares retains near-ground precision. A perpendicular distance
// discriminant also avoids inventing a thin shell interval at an exact tangent.
std::pair<double,double> roots(double radius,const PreparedRay& ray,double boundary) {
    const double b=ray.b,c=(ray.height-boundary)*(2*radius+ray.height+boundary);
    const double disc=(radius+boundary-ray.perpendicular)*(radius+boundary+ray.perpendicular);
    if(disc<0) return {1,-1};
    if(disc==0) return {-b,-b};
    const double q=-b-std::copysign(std::sqrt(disc),b);
    if(q==0) return {0,0};
    const double other=c/q;
    return {std::min(q,other),std::max(q,other)};
}
Result<PreparedRay> prepare(const AtmosphereParameters& a,const PathMediumRay& ray) {
    if(!position_valid(ray.origin) || !ray.direction.allFinite() || !bounded(ray.distance_m,0,2e7))
        return std::unexpected(invalid("Path medium ray outside finite world/distance domain"));
    const double scale=ray.direction.cwiseAbs().maxCoeff();
    if(scale==0) return std::unexpected(invalid("Path medium ray direction is zero"));
    const Vec3d d=(ray.direction/scale).normalized(),delta=ray.origin-vector(a.planet_center_m);
    if(!position_valid(ray.origin+d*ray.distance_m))
        return std::unexpected(invalid("Path medium ray endpoint outside world domain"));
    const double radius=delta.stableNorm(),h=radius-a.ground_radius_m;
    if(h<0) return std::unexpected(invalid("Path medium ray starts below ground"));
    return PreparedRay{d,h,delta.dot(d),delta.cross(d).stableNorm()};
}
double height_at(double radius,const PreparedRay& ray,double t) {
    const double r=radius+ray.height,delta=t*(t+2*ray.b);
    return ray.height+delta/(std::sqrt(std::max(0.,r*r+delta))+r);
}
}
Result<PathMediumSnapshot> PathMediumSnapshot::create(const EnvironmentDescription& input,
    const PathMediumSettings& settings,std::shared_ptr<const CloudFieldData> field) {
    auto normalized=normalize_environment(input);
    if(!normalized) return std::unexpected(normalized.error());
    const auto& e=*normalized; const auto& a=e.atmosphere; const auto& c=e.clouds;
    if(settings.model_version!=1 || !bounded(settings.time_seconds,-1e9,1e9) ||
        !bounded(settings.cloud_albedo,0,1) || !bounded(settings.cloud_anisotropy,-.95,.95))
        return std::unexpected(invalid("Invalid path medium settings or model version"));
    if(a.ground_radius_m>1e7 || a.atmosphere_height_m>1e6 || vector(a.planet_center_m).cwiseAbs().maxCoeff()>1e8)
        return std::unexpected(Error{ErrorCode::not_supported,"Path medium outside CPU geometry domain"});
    PathMediumSnapshot result; result.environment_=e; result.settings_=settings;
    result.clouds_=c.enabled && c.coverage>0 && c.density>0 && c.extinction_per_m>0;
    if(result.clouds_) {
        if(!field || field->key()!=cloud_field_key(c) || !field->bytes())
            return std::unexpected(invalid("Path medium requires a matching immutable cloud field"));
        result.field_=std::move(field);
    }
    // Upward rounding at each positive addition keeps this a conservative bound.
    auto add=[&](const Vec3d& coefficient) {
        for(int i=0;i<3;++i) if(coefficient[i]>0)
            result.majorant_[i]=std::nextafter(result.majorant_[i]+coefficient[i],std::numeric_limits<double>::infinity());
    };
    if(e.enabled) {
        add(vector(a.rayleigh_scattering_per_m)); add(vector(a.mie_extinction_per_m)); add(vector(a.absorption_extinction_per_m));
    }
    if(result.clouds_) add(Vec3d::Constant(std::nextafter(c.density*c.extinction_per_m,std::numeric_limits<double>::infinity())));
    result.valid_=true;
    return result;
}
Result<PathMediumCoefficients> PathMediumSnapshot::sample_at(const Vec3d& position,double h) const {
    PathMediumCoefficients result; const auto& e=environment_; const auto& a=e.atmosphere;
    if(h>=a.atmosphere_height_m) return result;
    h=std::max(0.,h);
    if(e.enabled) {
        const double mie_density=std::exp(-h/a.mie_scale_height_m);
        result.rayleigh_scattering=vector(a.rayleigh_scattering_per_m)*std::exp(-h/a.rayleigh_scale_height_m);
        result.mie_scattering=vector(a.mie_scattering_per_m)*mie_density;
        result.extinction=result.rayleigh_scattering+vector(a.mie_extinction_per_m)*mie_density+
            vector(a.absorption_extinction_per_m)*std::max(0.,1-std::abs(h-a.absorption_center_height_m)/a.absorption_half_width_m);
    }
    if(clouds_ && h>e.clouds.bottom_height_m && h<e.clouds.top_height_m) {
        auto density=sample_cloud_density(e,*field_,{position.x(),position.y(),position.z()},settings_.time_seconds);
        if(!density) return std::unexpected(density.error());
        const double sigma=*density*e.clouds.extinction_per_m;
        result.extinction.array()+=sigma; result.cloud_scattering=sigma*settings_.cloud_albedo;
    }
    return result;
}
Result<PathMediumCoefficients> PathMediumSnapshot::sample(const Vec3d& position) const {
    if(!valid_) return std::unexpected(invalid("Invalid path medium snapshot"));
    if(!position_valid(position)) return std::unexpected(invalid("Path medium point outside world domain"));
    const double h=(position-vector(environment_.atmosphere.planet_center_m)).stableNorm()-environment_.atmosphere.ground_radius_m;
    if(h<0) return std::unexpected(invalid("Path medium point below ground"));
    return sample_at(position,h);
}
Result<PathMediumSegments> PathMediumSnapshot::intervals(const PathMediumRay& input) const {
    if(!valid_) return std::unexpected(invalid("Invalid path medium snapshot"));
    const auto& e=environment_; const auto& a=e.atmosphere;
    auto ray=prepare(a,input); if(!ray) return std::unexpected(ray.error());
    PathMediumSegments result;
    if(input.distance_m==0 || (!e.enabled && !e.clouds.enabled)) return result;
    double limit=input.distance_m;
    const auto ground=roots(a.ground_radius_m,*ray,0);
    if(ray->b<0 && ground.first>=0 && ground.first<=ground.second && ground.first<=limit) {
        limit=ground.first; result.ground=true; result.ground_distance_m=limit;
    }
    const auto top=roots(a.ground_radius_m,*ray,a.atmosphere_height_m);
    const double begin=std::max(0.,top.first),end=std::min(limit,top.second);
    if(end<=begin) return result;
    std::array<double,6> cuts{}; std::size_t count=0; cuts[count++]=begin; cuts[count++]=end;
    if(clouds_) for(const double h:{e.clouds.bottom_height_m,e.clouds.top_height_m}) {
        const auto crossing=roots(a.ground_radius_m,*ray,h);
        if(crossing.first>crossing.second) continue;
        for(const double t:{crossing.first,crossing.second}) if(t>begin && t<end) cuts[count++]=t;
    }
    std::sort(cuts.begin(),cuts.begin()+count);
    for(std::size_t i=1;i<count;++i) {
        const double first=cuts[i-1],last=cuts[i]; if(last<=first) continue;
        const double h=height_at(a.ground_radius_m,*ray,(first+last)*.5);
        const bool cloud=clouds_ && h>e.clouds.bottom_height_m && h<e.clouds.top_height_m;
        if(!e.enabled && !cloud) continue;
        result.segments[result.count++]={first,last,cloud};
    }
    return result;
}
Result<PathMediumTransmittance> PathMediumSnapshot::integrate(const PathMediumRay& input,
    const PathMediumIntegration& settings,std::stop_token stop) const {
    if(!settings.steps || settings.steps>8192 || !settings.max_samples || settings.max_samples>40960)
        return std::unexpected(invalid("Invalid path medium integration settings"));
    auto segments=intervals(input); if(!segments) return std::unexpected(segments.error());
    if(std::uint64_t{segments->count}*settings.steps>settings.max_samples)
        return std::unexpected(invalid("Path medium integration sample budget exceeded"));
    if(stop.stop_requested()) return std::unexpected(cancelled());
    const auto& a=environment_.atmosphere; auto ray=prepare(a,input); if(!ray) return std::unexpected(ray.error());
    PathMediumTransmittance result; result.ground=segments->ground;
    for(std::uint32_t k=0;k<segments->count;++k) {
        const auto segment=segments->segments[k];
        auto node=[&](std::uint32_t i) {
            const double u=double(i)/settings.steps;
            return segment.clouds ? u : .5-.5*std::cos(std::numbers::pi*u);
        };
        for(std::uint32_t i=0;i<settings.steps;++i) {
            if(stop.stop_requested()) return std::unexpected(cancelled());
            const double first=std::lerp(segment.begin_m,segment.end_m,node(i)),last=std::lerp(segment.begin_m,segment.end_m,node(i+1));
            const double t=(first+last)*.5;
            auto coefficient=sample_at(input.origin+ray->direction*t,height_at(a.ground_radius_m,*ray,t));
            if(!coefficient) return std::unexpected(coefficient.error());
            result.optical_depth+=coefficient->extinction*(last-first); ++result.samples;
        }
    }
    result.transmittance=(-result.optical_depth.array()).exp().matrix();
    if(!result.optical_depth.allFinite() || !result.transmittance.allFinite() ||
        (result.transmittance.array()<0).any() || (result.transmittance.array()>1).any())
        return std::unexpected(Error{ErrorCode::internal_error,"Nonfinite path medium integral"});
    return result;
}
} // namespace dk::render

#include <dk/render/VolumePathReference.hpp>
#include "PathReferenceInternal.hpp"
#include "PathBsdf.hpp"
#include "VolumePhase.hpp"
#include <algorithm>
#include <cmath>
#include <limits>

namespace dk::render {
namespace detail {
struct VolumeImageState {
    explicit VolumeImageState(memory::ResourceHandle heap) : colors(0,memory::Allocator<Vec3d>{heap}),moments(0,memory::Allocator<Vec3d>{heap}) {}
    VolumePathImageInfo info;
    PathTraceScene source;
    PathMediumSnapshot medium;
    Vector<Vec3d> colors,moments;
};
struct VolumeReferenceAccess {
    static VolumePathImage image(std::shared_ptr<const VolumeImageState> s) { return VolumePathImage{std::move(s)}; }
};
}
namespace {
using namespace detail;
constexpr double pi=std::numbers::pi,infinity=std::numeric_limits<double>::max();
template<class T> T take(Result<T> value) { if(!value) throw value.error(); return std::move(*value); }
void check(Result<void> value) { if(!value) throw value.error(); }
Vec3d vector(const EnvironmentVector& v) { return {v[0],v[1],v[2]}; }
Error cancelled() { return {ErrorCode::invalid_state,"CPU volume path tracing cancelled",{"render.pathtracing.volume.cancelled"}}; }
std::uint32_t mix(std::uint32_t x) { x^=x>>16; x*=0x7feb352du; x^=x>>15; x*=0x846ca68bu; return x^(x>>16); }
double power(double a,double b) { if(a<=0) return 0; if(b<=0) return 1; const double q=b/a; return 1/(1+q*q); }
const PathSceneState& validate(const PathTraceScene& scene,const PathMediumSnapshot& medium,const VolumePathSettings& s,
    PathTraceSamples samples,std::uint64_t pixels) {
    const auto* state=PathReferenceAccess::get(scene);
    if(!state || !medium) throw path_invalid("Volume path requires valid frozen scene and medium");
    if(state->source.environment()!=medium.environment()) throw path_invalid("Volume scene and medium environment mismatch");
    if(s.model_version!=4 || s.sampler_version!=2 || s.max_bounces>32 || s.rr_start>s.max_bounces ||
        !s.environment_rgb.allFinite() || (s.environment_rgb.array()<0).any() || (s.environment_rgb.array()>1e6).any() ||
        (s.estimator!=PathTraceEstimator::mis && s.estimator!=PathTraceEstimator::bsdf_only) ||
        !samples.count || samples.count>65536 || samples.first>65536-samples.count || !pixels || pixels>4096 ||
        !s.max_rays || s.max_rays>(1ull<<24) || !s.max_triangle_tests || s.max_triangle_tests>(1ull<<29) ||
        !s.max_tracking_steps || s.max_tracking_steps>(1ull<<28) || !s.max_steps_per_path || s.max_steps_per_path>(1u<<20))
        throw path_invalid("Invalid volume path settings or sample/work interval");
    const auto rays=pixels*samples.count*(2ull*s.max_bounces+1);
    if(rays>s.max_rays || rays>s.max_triangle_tests/std::max<std::size_t>(1,state->triangles.size()))
        throw path_invalid("CPU volume geometry work budget exceeded");
    return *state;
}
struct Random {
    std::uint64_t seed;
    std::uint32_t pixel,sample,event=0;
    double operator()(VolumeSampleStream stream,std::uint32_t counter=0) const {
        return volume_path_sample(seed,pixel,sample,event,stream,counter);
    }
};
struct Vertex {
    Vec3d position=Vec3d::Zero(),normal=Vec3d::Zero(),incoming=Vec3d::Zero();
    std::optional<PathTraceHit> hit;
    bool volume=false,ground=false;
    PathBsdf bsdf;
    VolumePhase phase;
};
struct Context {
    const PathSceneState& scene;
    const PathMediumSnapshot& medium;
    const VolumePathSettings& settings;
    VolumePathStatistics& stats;
    std::stop_token stop;
    bool planet=false;
    Vec3d sun_direction,sun_radiance,majorant,air_majorant;
    double cone_width=0,sun_pdf=0,class_pdf=0;
    bool environment_light=false,sun_light=false;
    std::uint32_t steps=0;
    Context(const PathSceneState& p,const PathMediumSnapshot& m,const VolumePathSettings& s,VolumePathStatistics& count,std::stop_token token)
        : scene(p),medium(m),settings(s),stats(count),stop(token) {
        const auto& e=m.environment(); planet=e.enabled || e.clouds.enabled;
        majorant=m.extinction_majorant(); sun_direction=vector(e.sun.direction);
        auto air=e; air.clouds.enabled=false;
        air_majorant=take(PathMediumSnapshot::create(air)).extinction_majorant();
        const double alpha=e.sun.angular_radius_rad;
        cone_width=2*std::pow(std::sin(alpha*.5),2); sun_pdf=1/(2*pi*cone_width);
        sun_radiance=s.sun ? vector(take(solar_disk_radiance(e.sun))) : Vec3d::Zero();
        environment_light=s.environment_rgb.maxCoeff()>0; sun_light=sun_radiance.maxCoeff()>0;
        const int lights=int(environment_light)+int(sun_light)+int(scene.light_weight>0);
        class_pdf=lights ? 1./lights : 0;
    }
    void cancellation() const { if(stop.stop_requested()) throw cancelled(); }
    void tracking_step() {
        cancellation();
        if(stats.tracking_steps==settings.max_tracking_steps || steps==settings.max_steps_per_path)
            throw path_invalid("CPU volume tracking work budget exceeded");
        ++stats.tracking_steps; ++steps;
    }
    std::optional<PathTraceHit> nearest(const PathTraceRay& ray) {
        cancellation();
        if(stats.rays==settings.max_rays || scene.triangles.size()>settings.max_triangle_tests-stats.triangle_tests)
            throw path_invalid("CPU volume geometry work budget exceeded");
        PathTraceStatistics count; auto hit=nearest_path(scene,ray,count);
        stats.rays+=count.rays; stats.triangle_tests+=count.triangle_tests; return hit;
    }
    PathMediumSegments intervals(const PathTraceRay& ray,double maximum) const {
        if(!planet) return {};
        const auto& a=medium.environment().atmosphere;
        const Vec3d delta=ray.origin-vector(a.planet_center_m);
        const double radius=delta.stableNorm(),h=radius-a.ground_radius_m,b=delta.dot(ray.direction);
        if(h<0) throw path_invalid("Volume path starts below ground");
        const double perpendicular=delta.cross(ray.direction).stableNorm(),outer=a.ground_radius_m+a.atmosphere_height_m;
        const double discriminant=(outer-perpendicular)*(outer+perpendicular);
        double exit=0;
        if(discriminant>=0) {
            const double q=-b-std::copysign(std::sqrt(discriminant),b);
            const double c=(h-a.atmosphere_height_m)*(2*a.ground_radius_m+h+a.atmosphere_height_m);
            exit=q==0 ? 0 : std::max(q,c/q);
        }
        const double limit=std::min(maximum,std::max(0.,exit));
        const auto segments=take(medium.intervals({ray.origin,ray.direction,std::min(limit,2e7)}));
        // Ground can end the physical path long before the far side of the outer sphere.
        // The capped query is usable only when it proves that nearer opaque endpoint.
        if(limit>2e7 && !segments.ground)
            throw Error{ErrorCode::not_supported,"Volume medium segment exceeds 2e7 metre reference domain"};
        return segments;
    }
    Vec3d above_ground(Vec3d origin) const {
        if(!planet) return origin;
        const auto& a=medium.environment().atmosphere;
        const Vec3d center=vector(a.planet_center_m),delta=origin-center;
        const double error=128*std::numeric_limits<double>::epsilon()*std::max(1.,center.cwiseAbs().maxCoeff()+a.ground_radius_m);
        if(delta.stableNorm()<a.ground_radius_m+error) origin=center+delta.normalized()*(a.ground_radius_m+error);
        return origin;
    }
    Vec3d origin(const Vertex& v,const Vec3d& wi) const {
        if(v.volume) return v.position;
        return above_ground(v.hit ? offset_path_origin(scene.triangles[v.hit->index],v.position,wi) : v.position);
    }
    double area_pdf(std::size_t index,const Vec3d& from,const Vec3d& point) const {
        const auto& t=scene.triangles[index]; if(t.light_weight<=0) return 0;
        const Vec3d d=point-from; const double length=d.norm(); if(length==0) return 0;
        double cosine=-t.normal.dot(d/length); if(t.double_sided) cosine=std::abs(cosine);
        return cosine<=0 ? 0 : class_pdf*(t.light_weight/scene.light_weight)*length*length/(t.area*cosine);
    }
    PathMediumCoefficients coefficient(const Vec3d& p,unsigned channel,double rate) const {
        const auto c=take(medium.sample(p));
        if(c.extinction[channel]>rate || c.extinction[channel]<0)
            throw Error{ErrorCode::internal_error,"Volume extinction exceeds conservative majorant"};
        return c;
    }
    // 0: endpoint, 1: real scatter, 2: absorption. Null events retain this ray/event.
    int flight(const PathTraceRay& ray,const PathMediumSegments& segments,unsigned channel,const Random& random,Vertex& vertex) {
        std::uint32_t counter=0;
        for(unsigned k=0;k<segments.count;++k) {
            const double rate=segments.segments[k].clouds ? majorant[channel] : air_majorant[channel];
            if(rate==0) continue;
            double t=segments.segments[k].begin_m;
            while(true) {
                tracking_step(); const auto index=counter++;
                const double next=t-std::log1p(-random(VolumeSampleStream::free_path,index))/rate;
                if(!(next>t)) throw Error{ErrorCode::internal_error,"Nonadvancing volume free path"};
                t=next; if(t>=segments.segments[k].end_m) break;
                const Vec3d position=ray.origin+ray.direction*t; const auto c=coefficient(position,channel,rate);
                const double sigma_s=c.rayleigh_scattering[channel]+c.mie_scattering[channel]+c.cloud_scattering;
                const double choice=random(VolumeSampleStream::collision,index)*rate;
                if(choice<sigma_s) {
                    vertex.volume=true; vertex.position=position; vertex.incoming=ray.direction;
                    vertex.phase={{c.rayleigh_scattering[channel]/sigma_s,c.mie_scattering[channel]/sigma_s,c.cloud_scattering/sigma_s},
                        medium.environment().atmosphere.mie_anisotropy,medium.settings().cloud_anisotropy};
                    ++stats.medium_scatter; return 1;
                }
                if(choice<c.extinction[channel]) { ++stats.absorption; return 2; }
                ++stats.null_collisions;
            }
        }
        return 0;
    }
    double visibility(const Vertex& v,const Vec3d& wi,std::optional<std::size_t> light,const Vec3d& point,unsigned channel,const Random& random) {
        PathTraceRay ray{origin(v,wi),wi};
        if(light) {
            const Vec3d end=offset_path_origin(scene.triangles[*light],point,-wi),delta=end-ray.origin;
            const double distance=delta.norm(); if(distance<=0 || delta.dot(wi)<=0) return 0;
            ray.direction=delta/distance; ray.maximum=std::nextafter(distance,0.);
        }
        if(nearest(ray)) return 0;
        const auto segments=intervals(ray,ray.maximum); if(segments.ground) return 0;
        double transmittance=1; std::uint32_t counter=0;
        for(unsigned k=0;k<segments.count;++k) {
            const double rate=segments.segments[k].clouds ? majorant[channel] : air_majorant[channel];
            if(rate==0) continue;
            double t=segments.segments[k].begin_m;
            while(true) {
                tracking_step();
                const double next=t-std::log1p(-random(VolumeSampleStream::shadow_path,counter++))/rate;
                if(!(next>t)) throw Error{ErrorCode::internal_error,"Nonadvancing volume shadow path"};
                t=next; if(t>=segments.segments[k].end_m) break;
                transmittance*=1-coefficient(ray.origin+ray.direction*t,channel,rate).extinction[channel]/rate;
                if(transmittance==0) return 0; // Exact zero, no biased threshold cutoff.
            }
        }
        return transmittance;
    }
    std::pair<double,double> evaluate(const Vertex& v,const Vec3d& wi,unsigned channel) const {
        if(v.volume) { const double p=v.phase.eval(v.incoming.dot(wi)); return {p,p}; }
        const auto b=eval_path_bsdf(v.bsdf,v.normal,-v.incoming,wi);
        return {b.value[channel]*std::max(0.,v.normal.dot(wi)),b.pdf};
    }
    double direct(const Vertex& v,unsigned channel,const Random& random) {
        if(settings.estimator==PathTraceEstimator::bsdf_only || class_pdf==0) return 0;
        int selected=int(random(VolumeSampleStream::light_class)/class_pdf);
        double radiance=0,pdf=0; Vec3d wi=Vec3d::Zero(),point=Vec3d::Zero(); std::optional<std::size_t> light;
        if(environment_light && selected--==0) {
            wi=volume_direction(Vec3d::UnitY(),1-2*random(VolumeSampleStream::light_point),random(VolumeSampleStream::light_point,1));
            radiance=settings.environment_rgb[channel]; pdf=class_pdf/(4*pi);
        } else if(sun_light && selected--==0) {
            wi=volume_direction(sun_direction,1-cone_width*random(VolumeSampleStream::light_point),random(VolumeSampleStream::light_point,1));
            radiance=sun_radiance[channel]; pdf=class_pdf*sun_pdf;
        } else if(scene.light_weight>0) {
            const double target=random(VolumeSampleStream::light_point)*scene.light_weight;
            auto it=std::upper_bound(scene.triangles.begin(),scene.triangles.end(),target,[](double p,const PathTriangle& t) { return p<t.light_cdf; });
            if(it==scene.triangles.end()) return 0;
            light=std::size_t(it-scene.triangles.begin());
            const double root=std::sqrt(random(VolumeSampleStream::light_point,1)),u=random(VolumeSampleStream::light_point,2);
            point=it->a*(1-root)+it->b*(root*(1-u))+it->c*(root*u);
            const Vec3d delta=point-v.position; if(delta.squaredNorm()==0) return 0;
            wi=delta.normalized(); radiance=it->emission[channel]; pdf=area_pdf(*light,v.position,point);
        }
        if(pdf<=0 || radiance==0) return 0;
        const auto [factor,scatter_pdf]=evaluate(v,wi,channel); if(factor<=0) return 0;
        return radiance*factor*visibility(v,wi,light,point,channel,random)*power(pdf,scatter_pdf)/pdf;
    }
    Vec3d trace(PathTraceRay ray,std::uint32_t pixel,std::uint32_t sample) {
        cancellation(); steps=0; Random random{settings.seed,pixel,sample};
        const auto channel=unsigned(random(VolumeSampleStream::channel)*3);
        double value=0,beta=1,previous_pdf=0; Vec3d previous=Vec3d::Zero();
        const bool mis=settings.estimator==PathTraceEstimator::mis;
        unsigned medium_events=0;
        for(unsigned depth=0;depth<=settings.max_bounces;++depth) {
            cancellation(); random.event=depth;
            auto hit=nearest(ray); const auto segments=intervals(ray,hit ? hit->distance : infinity);
            Vertex v; const int event=flight(ray,segments,channel,random,v);
            if(event==2) break;
            if(event==1 && ++medium_events==2) ++stats.multiple_scatter_paths;
            if(event==0) {
                const bool ground=segments.ground && (!hit || segments.ground_distance_m<hit->distance);
                if(!hit && !ground) {
                    value+=beta*settings.environment_rgb[channel]*(depth && mis ? power(previous_pdf,class_pdf/(4*pi)) : 1);
                    if(sun_light && 1-std::clamp(ray.direction.dot(sun_direction),-1.,1.)<=cone_width)
                        value+=beta*sun_radiance[channel]*(depth && mis ? power(previous_pdf,class_pdf*sun_pdf) : 1);
                    break;
                }
                v.ground=ground; v.incoming=ray.direction;
                if(ground) {
                    ++stats.ground_hits;
                    v.position=ray.origin+ray.direction*segments.ground_distance_m;
                    v.normal=(v.position-vector(medium.environment().atmosphere.planet_center_m)).normalized();
                    v.bsdf={vector(medium.environment().atmosphere.ground_albedo),0,1,PathTraceMaterialModel::lambert};
                } else {
                    v.hit=hit; v.position=hit->position; v.normal=hit->normal; const auto& t=scene.triangles[hit->index];
                    v.bsdf={path_base_color(scene,t,hit->barycentric).head<3>(),t.metallic,t.roughness,scene.material_model};
                    value+=beta*t.emission[channel]*(depth && mis ? power(previous_pdf,area_pdf(hit->index,previous,hit->position)) : 1);
                }
            }
            if(depth==settings.max_bounces) break;
            value+=beta*direct(v,channel,random);
            Vec3d wi;
            if(v.volume) {
                wi=v.phase.sample(ray.direction,random(VolumeSampleStream::phase_component),random(VolumeSampleStream::phase_direction),random(VolumeSampleStream::phase_direction,1));
                previous_pdf=v.phase.eval(ray.direction.dot(wi));
            } else {
                const auto sampled=sample_path_bsdf(v.bsdf,v.normal,-ray.direction,random(VolumeSampleStream::bsdf),random(VolumeSampleStream::bsdf,1));
                if(sampled.evaluated.pdf<=0) break;
                wi=sampled.direction; previous_pdf=sampled.evaluated.pdf;
                beta*=sampled.evaluated.value[channel]*std::max(0.,v.normal.dot(wi))/previous_pdf; ++stats.surface_scatter;
            }
            if(!std::isfinite(beta) || beta<0) throw Error{ErrorCode::internal_error,"Nonfinite volume throughput"};
            if(beta==0) break;
            if(settings.rr_start && depth+1>=settings.rr_start) {
                const double survival=std::clamp(beta,.05,.95);
                if(random(VolumeSampleStream::roulette)>=survival) break;
                beta/=survival;
            }
            previous=v.position; ray={origin(v,wi),wi};
        }
        Vec3d result=Vec3d::Zero(); result[channel]=3*value;
        if(!result.allFinite() || value<0) throw Error{ErrorCode::internal_error,"Nonfinite volume radiance"};
        return result;
    }
};
struct Moments {
    Vec3d sum=Vec3d::Zero(),correction=Vec3d::Zero(),square=Vec3d::Zero(),square_correction=Vec3d::Zero();
    void add(const Vec3d& v) {
        const Vec3d y=v-correction,t=sum+y; correction=(t-sum)-y; sum=t;
        const Vec3d sy=v.cwiseProduct(v)-square_correction,st=square+sy; square_correction=(st-square)-sy; square=st;
        if(!sum.allFinite() || !square.allFinite()) throw Error{ErrorCode::internal_error,"Nonfinite volume moments"};
    }
};
}
double volume_path_sample(std::uint64_t seed,std::uint32_t pixel,std::uint32_t sample,std::uint32_t event,
    VolumeSampleStream stream,std::uint32_t counter) noexcept {
    auto h=mix(std::uint32_t(seed)^0x3c6ef372u); h=mix(h^std::uint32_t(seed>>32));
    h=mix(h^pixel); h=mix(h^sample); h=mix(h^event); h=mix(h^std::uint32_t(stream)); h=mix(h^counter);
    return (double(h>>8)+.5)/16777216.;
}
Result<VolumePathEstimate> estimate_volume_path_reference(const PathTraceScene& scene,const PathMediumSnapshot& medium,
    const PathTraceRay& ray,const VolumePathSettings& settings,PathTraceSamples samples,std::uint32_t pixel,std::stop_token stop) {
    try {
        const auto& input=validate(scene,medium,settings,samples,1); check(validate_path_ray(ray));
        VolumePathEstimate result; Context context{input,medium,settings,result.statistics,stop}; Moments moments;
        for(std::uint32_t s=samples.first;s<samples.first+samples.count;++s) moments.add(context.trace(ray,pixel,s));
        context.cancellation(); result.mean=moments.sum/samples.count; result.second_moment=moments.square/samples.count; return result;
    } catch(const Error& error) { return std::unexpected(error); }
    catch(const std::bad_alloc&) { return std::unexpected(Error{ErrorCode::internal_error,"Volume reference allocation failed"}); }
}
Result<VolumePathImage> render_volume_path_reference(memory::ResourceHandle heap,const PathTraceScene& scene,const PathMediumSnapshot& medium,
    const RenderView& view,const VolumePathSettings& settings,PathTraceSamples samples,std::stop_token stop) {
    try {
        const auto& d=view.description(); const auto pixels=std::uint64_t{d.width}*d.height;
        const auto& input=validate(scene,medium,settings,samples,pixels); check(validate_path_view(input,view));
        if(!heap || heap.state()!=memory::ResourceState::open) throw path_invalid("Volume image requires an open heap");
        if(stop.stop_requested()) throw cancelled();
        auto image=memory::make_shared_in<detail::VolumeImageState>(heap,heap);
        image->source=scene; image->medium=medium;
        image->info={scene.id(),scene.revision(),scene.fingerprint(),medium.field_hash(),scene.material_model(),d,
            medium.environment(),medium.settings(),settings,samples,{}};
        image->colors.resize(std::size_t(pixels)); image->moments.resize(std::size_t(pixels));
        Context context{input,medium,settings,image->info.statistics,stop};
        for(std::uint32_t y=0;y<d.height;++y) for(std::uint32_t x=0;x<d.width;++x) {
            const auto pixel=y*d.width+x; Moments moments;
            for(std::uint32_t s=samples.first;s<samples.first+samples.count;++s) {
                const Random random{settings.seed,pixel,s};
                const auto ray=take(path_trace_camera_ray(view,x,y,{random(VolumeSampleStream::pixel),random(VolumeSampleStream::pixel,1)}));
                moments.add(context.trace(ray,pixel,s));
            }
            image->colors[pixel]=moments.sum/samples.count; image->moments[pixel]=moments.square/samples.count;
        }
        context.cancellation(); return detail::VolumeReferenceAccess::image(std::move(image));
    } catch(const Error& error) { return std::unexpected(error); }
    catch(const std::bad_alloc&) { return std::unexpected(Error{ErrorCode::internal_error,"Volume image allocation failed"}); }
}
std::span<const Vec3d> VolumePathImage::linear_rgb() const noexcept { return state_ ? std::span<const Vec3d>{state_->colors} : std::span<const Vec3d>{}; }
std::span<const Vec3d> VolumePathImage::second_moments() const noexcept { return state_ ? std::span<const Vec3d>{state_->moments} : std::span<const Vec3d>{}; }
const VolumePathImageInfo& VolumePathImage::info() const noexcept { static const VolumePathImageInfo empty; return state_ ? state_->info : empty; }
} // namespace dk::render

#include "PathReferenceInternal.hpp"
#include "PathBsdf.hpp"
#include <Eigen/LU>
#include <algorithm>
#include <cmath>

namespace dk::render {
namespace {
using namespace detail;
std::uint32_t mix(std::uint32_t x) noexcept {
    x^=x>>16; x*=0x7feb352du; x^=x>>15; x*=0x846ca68bu; return x^(x>>16);
}
bool color_valid(const Vec3d& c) { return c.allFinite() && (c.array()>=0).all() && (c.array()<=1e6).all(); }
Result<void> work_valid(const PathSceneState& scene,std::uint64_t pixels,const PathTraceSettings& s,PathTraceSamples range) {
    if (s.model_version!=3 || s.sampler_version!=1 || !s.max_bounces || s.max_bounces>32 || s.rr_start>s.max_bounces ||
        !color_valid(s.environment_rgb) || (s.sun!=PathTraceSun::none && s.sun!=PathTraceSun::scene) ||
        (s.estimator!=PathTraceEstimator::mis && s.estimator!=PathTraceEstimator::bsdf_only) ||
        !range.count || range.count>65536 || range.first>65536-range.count ||
        !pixels || pixels>4096 || !s.max_rays || s.max_rays>(std::uint64_t{1}<<24) ||
        !s.max_triangle_tests || s.max_triangle_tests>(std::uint64_t{1}<<29))
        return std::unexpected(path_invalid("Invalid path settings, sample interval or budget"));
    const auto env=scene.source.environment();
    if ((env.enabled || env.clouds.enabled) && !s.surface_only)
        return std::unexpected(Error{ErrorCode::not_supported,"Scene media require explicit surface_only reference mode"});
    const Vec3d sun{env.sun.irradiance[0],env.sun.irradiance[1],env.sun.irradiance[2]};
    const Vec3d direction{env.sun.direction[0],env.sun.direction[1],env.sun.direction[2]};
    if (s.sun==PathTraceSun::scene && (!color_valid(sun) || !direction.allFinite() || std::abs(direction.squaredNorm()-1)>1e-10))
        return std::unexpected(path_invalid("Invalid frozen path sun"));
    // All factors have bounded ranges before multiplication.
    const auto rays=pixels*range.count*(2ull*s.max_bounces+1);
    if (rays>s.max_rays || rays>s.max_triangle_tests/std::max<std::uint64_t>(1,scene.triangles.size()))
        return std::unexpected(path_invalid("CPU path reference work budget exceeded"));
    return {};
}
double power_weight(double a,double b) {
    if (a<=0) return 0;
    if (b<=0) return 1;
    const double ratio=b/a;
    return 1/(1+ratio*ratio);
}
struct Lights {
    Vec3d sun, direction;
    bool environment=false, solar=false, area=false;
    double class_pdf=0;
};
Lights lights_for(const PathSceneState& scene,const PathTraceSettings& s) {
    const auto env=scene.source.environment();
    Lights l; l.sun={env.sun.irradiance[0],env.sun.irradiance[1],env.sun.irradiance[2]};
    l.direction={env.sun.direction[0],env.sun.direction[1],env.sun.direction[2]};
    l.environment=s.environment_rgb.maxCoeff()>0;
    l.solar=s.sun==PathTraceSun::scene && l.sun.maxCoeff()>0;
    l.area=scene.light_weight>0;
    const int count=int(l.environment)+int(l.solar)+int(l.area);
    l.class_pdf=count ? 1.0/count : 0;
    return l;
}
double area_pdf(const PathSceneState& scene,const PathTriangle& t,const Vec3d& from,const Vec3d& point,double class_pdf) {
    if (t.light_weight==0) return 0;
    const Vec3d delta=point-from; const double d2=delta.squaredNorm();
    if (d2==0) return 0;
    double cosine=-t.normal.dot(delta/std::sqrt(d2));
    if (t.double_sided) cosine=std::abs(cosine);
    if (cosine<=0) return 0;
    return class_pdf*(t.light_weight/scene.light_weight)*d2/(t.area*cosine);
}
bool visible(const PathSceneState& scene,const PathTraceHit& hit,const Vec3d& direction,PathTraceStatistics& stats,
    const PathTriangle* light=nullptr,const Vec3d& point=Vec3d::Zero()) {
    PathTraceRay ray; ray.origin=offset_path_origin(scene.triangles[hit.index],hit.position,direction);
    ray.direction=direction;
    if (light) {
        const Vec3d end=offset_path_origin(*light,point,-direction),delta=end-ray.origin;
        const double length=delta.norm();
        if (length==0 || delta.dot(direction)<=0) return false;
        ray.direction=delta/length; ray.maximum=std::nextafter(length,0.0);
    }
    return !nearest_path(scene,ray,stats);
}
Vec3d direct(const PathSceneState& scene,const PathTraceHit& hit,const PathBsdf& bsdf,const Vec3d& wo,const Lights& lights,const PathTraceSettings& s,
    std::uint32_t pixel,std::uint32_t sample,std::uint32_t bounce,PathTraceStatistics& stats) {
    auto random=[&](std::uint32_t dim) { return path_trace_sample(s.seed,pixel,sample,bounce,dim); };
    const bool bsdf_only=s.estimator==PathTraceEstimator::bsdf_only;
    if (bsdf_only && !lights.solar) return Vec3d::Zero();
    if (!bsdf_only && lights.class_pdf==0) return Vec3d::Zero();
    int selected=bsdf_only ? 0 : static_cast<int>(random(2)/lights.class_pdf);
    Vec3d wi=Vec3d::Zero(),radiance=Vec3d::Zero(),point=Vec3d::Zero();
    double pdf=0; bool delta=false; const PathTriangle* emitter=nullptr;
    if (!bsdf_only && lights.environment && selected--==0) {
        const double z=1-2*random(4),r=std::sqrt(std::max(0.0,1-z*z)),phi=2*path_pi*random(5);
        wi={r*std::cos(phi),r*std::sin(phi),z}; radiance=s.environment_rgb; pdf=lights.class_pdf/(4*path_pi);
    } else if (lights.solar && (bsdf_only || selected--==0)) {
        wi=lights.direction; radiance=lights.sun; pdf=bsdf_only ? 1 : lights.class_pdf; delta=true;
    } else if (lights.area) {
        const double target=random(3)*scene.light_weight;
        auto found=std::upper_bound(scene.triangles.begin(),scene.triangles.end(),target,
            [](double weight,const PathTriangle& item) { return weight<item.light_cdf; });
        if (found==scene.triangles.end()) return Vec3d::Zero();
        emitter=&*found;
        const double root=std::sqrt(random(4)),v=random(5);
        point=emitter->a*(1-root)+emitter->b*(root*(1-v))+emitter->c*(root*v);
        const Vec3d displacement=point-hit.position;
        if (displacement.squaredNorm()==0) return Vec3d::Zero();
        wi=displacement.normalized(); radiance=emitter->emission;
        pdf=area_pdf(scene,*emitter,hit.position,point,lights.class_pdf);
    }
    const double cosine=hit.normal.dot(wi);
    if (cosine<=0 || pdf<=0 || !visible(scene,hit,wi,stats,emitter,point)) return Vec3d::Zero();
    const auto evaluated=eval_path_bsdf(bsdf,hit.normal,wo,wi);
    const double weight=delta ? 1 : power_weight(pdf,evaluated.pdf);
    return evaluated.value.cwiseProduct(radiance)*(cosine*weight/pdf);
}
Result<Vec3d> trace_one(const PathSceneState& scene,PathTraceRay ray,const PathTraceSettings& s,const Lights& lights,
    std::uint32_t pixel,std::uint32_t sample,PathTraceStatistics& stats,std::stop_token cancel) {
    Vec3d radiance=Vec3d::Zero(),throughput=Vec3d::Ones(),previous=Vec3d::Zero();
    double bsdf_pdf=0;
    for (std::uint32_t depth=0;depth<=s.max_bounces;++depth) {
        if (cancel.stop_requested()) return std::unexpected(path_cancelled());
        const auto hit=nearest_path(scene,ray,stats);
        if (!hit) {
            const double weight=depth && s.estimator==PathTraceEstimator::mis ? power_weight(bsdf_pdf,lights.class_pdf/(4*path_pi)) : 1;
            radiance+=throughput.cwiseProduct(s.environment_rgb)*weight;
            break;
        }
        const auto& t=scene.triangles[hit->index];
        const Vec3d albedo=path_base_color(scene,t,hit->barycentric).head<3>();
        const PathBsdf bsdf{albedo,t.metallic,t.roughness,scene.material_model};
        const double weight=depth && s.estimator==PathTraceEstimator::mis ?
            power_weight(bsdf_pdf,area_pdf(scene,t,previous,hit->position,lights.class_pdf)) : 1;
        radiance+=throughput.cwiseProduct(t.emission)*weight;
        if (depth==s.max_bounces || (scene.material_model==PathTraceMaterialModel::lambert && albedo.maxCoeff()==0)) break;
        radiance+=throughput.cwiseProduct(direct(scene,*hit,bsdf,-ray.direction,lights,s,pixel,sample,depth,stats));
        const auto sampled=sample_path_bsdf(bsdf,hit->normal,-ray.direction,path_trace_sample(s.seed,pixel,sample,depth,6),path_trace_sample(s.seed,pixel,sample,depth,7));
        if(sampled.evaluated.pdf<=0) break;
        const Vec3d wi=sampled.direction;
        bsdf_pdf=sampled.evaluated.pdf; previous=hit->position;
        throughput=throughput.cwiseProduct(sampled.evaluated.value)*(std::max(0.0,hit->normal.dot(wi))/bsdf_pdf);
        if(throughput.maxCoeff()==0) break;
        if (s.rr_start && depth+1>=s.rr_start) {
            const double survival=std::clamp(throughput.maxCoeff(),.05,.95);
            if (path_trace_sample(s.seed,pixel,sample,depth,8)>=survival) break;
            throughput/=survival;
        }
        ray={offset_path_origin(t,hit->position,wi),wi};
    }
    if (!radiance.allFinite() || (radiance.array()<0).any())
        return std::unexpected(Error{ErrorCode::internal_error,"Nonfinite path radiance"});
    return radiance;
}
struct Camera {
    Mat4d inverse_projection,world;
    std::uint32_t width=0,height=0;
    PathTraceRay ray(std::uint32_t x,std::uint32_t y,const Vec2d& subpixel) const {
        const double px=2*(double(x)+subpixel.x())/width-1,py=2*(double(y)+subpixel.y())/height-1;
        Vec4d near=inverse_projection*Vec4d{px,py,0,1},far=inverse_projection*Vec4d{px,py,1,1};
        near/=near.w(); far/=far.w();
        const Vec3d origin=world.block<3,1>(0,3);
        const Vec3d near_world=(world*near).head<3>(),far_world=(world*far).head<3>();
        const Vec3d direction=(near_world-origin).normalized();
        return {origin,direction,(near_world-origin).dot(direction),(far_world-origin).dot(direction)};
    }
};
Result<Camera> camera_for(const RenderView& view) {
    const auto& d=view.description(); const Mat4d& p=d.projection;
    const Mat3d rotation=d.camera_world.matrix().block<3,3>(0,0);
    if (!d.width || !d.height || d.width>64 || d.height>64 ||
        !(rotation.transpose()*rotation).isApprox(Mat3d::Identity(),1e-10) || std::abs(rotation.determinant()-1)>1e-10 ||
        p(3,0)!=0 || p(3,1)!=0 || p(3,2)!=-1 || p(3,3)!=0 || p(0,0)<=0 || p(1,1)==0 ||
        p(0,1)!=0 || p(0,3)!=0 || p(1,0)!=0 || p(1,3)!=0 || p(2,0)!=0 || p(2,1)!=0 ||
        p(2,2)>=-1 || p(2,3)>=0)
        return std::unexpected(path_invalid("Reference requires <=64x64 rigid finite forward-Z-depth perspective camera"));
    Camera camera{p.inverse(),d.camera_world.matrix(),d.width,d.height};
    for (const Vec2d corner: {Vec2d{0,0},Vec2d{1,1}}) {
        auto ray=camera.ray(corner.x()==0 ? 0 : d.width-1,corner.y()==0 ? 0 : d.height-1,corner);
        if (auto valid=validate_path_ray(ray);!valid) return std::unexpected(valid.error());
    }
    return camera;
}
bool same_source(const RenderScene& a,const RenderScene& b) {
    if (a.id()!=b.id() || a.revision()!=b.revision() || a.environment()!=b.environment() || a.entities().size()!=b.entities().size()) return false;
    for (std::size_t i=0;i<a.entities().size();++i) {
        if (a.entities()[i].id!=b.entities()[i].id || a.entities()[i].world.matrix()!=b.entities()[i].world.matrix()) return false;
        auto x=a.assets(i),y=b.assets(i);
        if (!x || !y || !std::ranges::equal(*x,*y)) return false;
    }
    return true;
}
}
Result<void> detail::validate_path_work(const PathSceneState& scene,std::uint64_t pixels,const PathTraceSettings& settings,PathTraceSamples samples) {
    return work_valid(scene,pixels,settings,samples);
}
Result<void> detail::validate_path_view(const PathSceneState& scene,const RenderView& view) {
    if (!same_source(scene.source,view.scene())) return std::unexpected(path_invalid("Path scene/view snapshot mismatch"));
    auto camera=camera_for(view); if (!camera) return std::unexpected(camera.error()); return {};
}
double path_trace_sample(std::uint64_t seed,std::uint32_t pixel,std::uint32_t sample,std::uint32_t bounce,std::uint32_t dimension) noexcept {
    auto h=mix(static_cast<std::uint32_t>(seed)^0xa511e9b3u);
    h=mix(h^static_cast<std::uint32_t>(seed>>32)); h=mix(h^pixel); h=mix(h^sample); h=mix(h^bounce); h=mix(h^dimension);
    return (double(h>>8)+.5)/16777216.0;
}
Result<PathTraceRay> path_trace_camera_ray(const RenderView& view,std::uint32_t x,std::uint32_t y,const Vec2d& subpixel) {
    auto camera=camera_for(view); if (!camera) return std::unexpected(camera.error());
    if (x>=camera->width || y>=camera->height || !subpixel.allFinite() || (subpixel.array()<0).any() || (subpixel.array()>=1).any())
        return std::unexpected(path_invalid("Invalid reference camera pixel/sample"));
    const auto ray=camera->ray(x,y,subpixel);
    if (auto valid=validate_path_ray(ray);!valid) return std::unexpected(valid.error());
    return ray;
}
Result<PathTraceEstimate> estimate_path_reference(const PathTraceScene& scene,const PathTraceRay& ray,const PathTraceSettings& s,
    PathTraceSamples samples,std::uint32_t pixel,std::stop_token cancel) {
    const auto* state=detail::PathReferenceAccess::get(scene);
    if (!state) return std::unexpected(Error{ErrorCode::invalid_state,"Empty path scene"});
    if (auto valid=validate_path_ray(ray);!valid) return std::unexpected(valid.error());
    if (auto valid=work_valid(*state,1,s,samples);!valid) return std::unexpected(valid.error());
    const auto lights=lights_for(*state,s); PathTraceEstimate result;
    for (std::uint32_t i=samples.first;i<samples.first+samples.count;++i) {
        auto color=trace_one(*state,ray,s,lights,pixel,i,result.statistics,cancel);
        if (!color) return std::unexpected(color.error());
        result.mean+=*color; result.second_moment+=color->cwiseProduct(*color);
    }
    result.mean/=samples.count; result.second_moment/=samples.count;
    if (!result.mean.allFinite() || !result.second_moment.allFinite())
        return std::unexpected(Error{ErrorCode::internal_error,"Nonfinite path estimate moments"});
    if (cancel.stop_requested()) return std::unexpected(path_cancelled());
    return result;
}
Result<PathTraceImage> render_path_reference(memory::ResourceHandle heap,const PathTraceScene& scene,const RenderView& view,
    const PathTraceSettings& s,PathTraceSamples samples,std::stop_token cancel) {
    const auto* state=detail::PathReferenceAccess::get(scene);
    if (!heap || heap.state()!=memory::ResourceState::open || !state)
        return std::unexpected(Error{ErrorCode::invalid_state,"Rendering requires open heap and path scene"});
    if (!same_source(state->source,view.scene())) return std::unexpected(path_invalid("Path scene/view snapshot mismatch"));
    auto camera=camera_for(view); if (!camera) return std::unexpected(camera.error());
    const auto pixels=std::uint64_t{camera->width}*camera->height;
    if (auto valid=work_valid(*state,pixels,s,samples);!valid) return std::unexpected(valid.error());
    if (cancel.stop_requested()) return std::unexpected(path_cancelled());
    try {
        auto image=memory::make_shared_in<detail::PathImageState>(heap,heap);
        image->info={camera->width,camera->height,scene.id(),scene.revision(),scene.fingerprint(),scene.texture_count(),scene.masked_emitter_count(),scene.material_model(),view.description(),s,samples,{}};
        image->pixels.resize(static_cast<std::size_t>(pixels),Vec3d::Zero());
        const auto lights=lights_for(*state,s);
        for (std::uint32_t y=0;y<camera->height;++y) for (std::uint32_t x=0;x<camera->width;++x) {
            const auto pixel=y*camera->width+x; Vec3d sum=Vec3d::Zero();
            for (std::uint32_t i=samples.first;i<samples.first+samples.count;++i) {
                const auto ray=camera->ray(x,y,{path_trace_sample(s.seed,pixel,i,0,0),path_trace_sample(s.seed,pixel,i,0,1)});
                auto color=trace_one(*state,ray,s,lights,pixel,i,image->info.statistics,cancel);
                if (!color) return std::unexpected(color.error());
                sum+=*color;
            }
            image->pixels[pixel]=sum/samples.count;
            if (!image->pixels[pixel].allFinite()) return std::unexpected(Error{ErrorCode::internal_error,"Nonfinite path image"});
        }
        if (cancel.stop_requested()) return std::unexpected(path_cancelled());
        return detail::PathReferenceAccess::image(std::move(image));
    } catch (const std::bad_alloc&) {
        return std::unexpected(Error{ErrorCode::internal_error,"Path image allocation failed"});
    }
}
std::span<const Vec3d> PathTraceImage::linear_rgb() const noexcept { return state_ ? std::span<const Vec3d>{state_->pixels} : std::span<const Vec3d>{}; }
const PathTraceImageInfo& PathTraceImage::info() const noexcept { static const PathTraceImageInfo empty; return state_ ? state_->info : empty; }
} // namespace dk::render

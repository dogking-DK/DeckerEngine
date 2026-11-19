#include "PathVolumeProbe.hpp"
#include <array>

namespace dk::test::path_probe {
void medium_acceleration(memory::ResourceHandle heap,PathTraceRenderer& r,SubmissionQueue& q) {
    Fixture f{heap}; auto e=cloud_environment(); e.clouds.weather_resolution=e.clouds.base_resolution=32;
    e.enabled=true; e.clouds.coverage=.65; e.clouds.extinction_per_m=.0008;
    CloudFieldCache cache; auto fields=take(cache.prepare(e.clouds));
    PathMediumSettings ms; ms.time_seconds=17; ms.cloud_albedo=.9;
    check(f.document->set_environment(e)); auto gpu=take(r.prepare_scene(q,f.scene()));
    auto medium=take(PathMediumSnapshot::create(e,ms,fields)); auto legacy=air_settings(4);
    legacy.sun=true; legacy.environment_rgb=Vec3d::Constant(.2); legacy.gpu_density_culling=false;
    auto culled=legacy; culled.gpu_density_culling=true;
    const std::array rays={PathTraceRay{{700,100,-500},{0,1,0}},PathTraceRay{{700,2200,-500},Vec3d{1,.02,0}.normalized()},
        PathTraceRay{{700,6000,-500},{0,-1,0}},PathTraceRay{{-10000,2200,5000},Vec3d{1,-.01,-.2}.normalized()}};
    for(const auto& ray:rays) {
        const auto a=cloud_image(r,q,gpu,medium,legacy,4096,ray),b=cloud_image(r,q,gpu,medium,culled,4096,ray);
        compare_images(a,b,1e-6);
        require(a.info().medium->tracking_steps==b.info().medium->tracking_steps,"zero culling changed random tracking sequence");
    }
    // An independent deterministic integral constrains local stochastic free flight.
    ms.cloud_albedo=0; e.enabled=false; check(f.document->set_environment(e)); gpu=take(r.prepare_scene(q,f.scene()));
    medium=take(PathMediumSnapshot::create(e,ms,fields)); auto accelerated=air_settings(0);
    accelerated.gpu_local_majorants=true; accelerated.environment_rgb=Vec3d::Ones();
    for(const auto& ray:std::span{rays}.first(2)) {
        const auto oracle=take(medium.integrate({ray.origin,ray.direction,200000},{8192,40960})).transmittance;
        close_color(oracle,take(medium.integrate({ray.origin,ray.direction,200000},{4096,40960})).transmittance,1e-4);
        const auto p=cloud_population(r,q,gpu,medium,ray,accelerated,65536,2,&oracle); cloud_check(p,false);
        require((p.mean-p.reference).norm()/std::max(1e-4,p.reference.norm())<=.02,"local mean nRMSE exceeds 2%");
    }
    // Production local tracking must retain real scatter, mixed air and low-sun NEE.
    ms.cloud_albedo=.9; e.enabled=true; e.sun.direction={1,.02,.1}; e.sun.angular_radius_rad=.1;
    check(f.document->set_environment(e)); gpu=take(r.prepare_scene(q,f.scene())); medium=take(PathMediumSnapshot::create(e,ms,fields));
    accelerated=culled; accelerated.gpu_local_majorants=true;
    const auto n=cloud_population(r,q,gpu,medium,rays[1],accelerated,16384,2);
    const auto four=cloud_population(r,q,gpu,medium,rays[1],accelerated,65536,2); cloud_check(four,false);
    require((four.mean-four.reference).norm()/std::max(1e-4,four.reference.norm())<=.02,"local radiance nRMSE exceeds 2%");
    require((n.reference-four.reference).norm()/std::max(1e-4,four.reference.norm())<=.005,"CPU N/4N reference not converged");
    std::puts("zero culling exact sequence; local absorption and mixed low-sun 8-seed oracle passed");
}
} // namespace dk::test::path_probe

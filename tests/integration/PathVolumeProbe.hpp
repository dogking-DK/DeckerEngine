#pragma once
#include "PathProbeSupport.hpp"

namespace dk::test::path_probe {
struct CloudPopulation {
    Vec3d mean=Vec3d::Zero(),reference=Vec3d::Zero(),error=Vec3d::Zero(),reference_error=Vec3d::Zero();
    double mse=0,rmse=0; std::uint64_t multiple=0;
};
EnvironmentDescription air_environment(double extinction=2e-5,double scattering=1e-5);
VolumePathSettings air_settings(unsigned depth=1);
PathTraceGpuImage air_image(PathTraceRenderer& r,SubmissionQueue& q,const PathTraceGpuScene& scene,const PathMediumSnapshot& m,
    VolumePathSettings s,unsigned samples,const PathTraceRay& ray,unsigned pixels=1);
EnvironmentDescription cloud_environment();
PathTraceGpuImage cloud_image(PathTraceRenderer& r,SubmissionQueue& q,const PathTraceGpuScene& scene,const PathMediumSnapshot& m,
    VolumePathSettings s,unsigned samples,const PathTraceRay& ray,unsigned pixels=1);
CloudPopulation cloud_population(PathTraceRenderer& r,SubmissionQueue& q,const PathTraceGpuScene& scene,const PathMediumSnapshot& m,
    const PathTraceRay& ray,VolumePathSettings s,unsigned samples,unsigned pixels,const Vec3d* expected=nullptr);
void cloud_check(const CloudPopulation& p,bool paired=true);

void air_numeric(memory::ResourceHandle heap,PathTraceRenderer& r,SubmissionQueue& q);
void air_statistics(memory::ResourceHandle heap,PathTraceRenderer& r,SubmissionQueue& q);
void air_progressive(memory::ResourceHandle heap,PathTraceRenderer& r,SubmissionQueue& q);
void air_failure(memory::ResourceHandle heap,PathTraceRenderer& r,SubmissionQueue& q);
void air_resources(memory::ResourceHandle heap,SubmissionQueue& q);

void cloud_numeric(memory::ResourceHandle heap,PathTraceRenderer& r,SubmissionQueue& q);
void cloud_statistics(memory::ResourceHandle heap,PathTraceRenderer& r,SubmissionQueue& q);
void cloud_surface(memory::ResourceHandle heap,PathTraceRenderer& r,SubmissionQueue& q);
void cloud_progressive(memory::ResourceHandle heap,PathTraceRenderer& r,SubmissionQueue& q,bool local=false);
void cloud_failure(memory::ResourceHandle heap,SubmissionQueue& q,bool local=false);

void medium_acceleration(memory::ResourceHandle heap,PathTraceRenderer& r,SubmissionQueue& q);

void orthographic(memory::ResourceHandle heap,PathTraceRenderer& r,SubmissionQueue& q);
void orthographic_medium(memory::ResourceHandle heap,PathTraceRenderer& r,SubmissionQueue& q);
} // namespace dk::test::path_probe

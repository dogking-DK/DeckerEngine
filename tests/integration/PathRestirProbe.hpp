#pragma once
#include "PathProbeSupport.hpp"
#include <array>

namespace dk::test::path_probe {
void compare_restir_statistics(const PathTraceGpuImageInfo& actual,const PathTraceGpuImageInfo& expected);
Vec3d temporal_furnace_oracle(std::uint64_t seed,unsigned count,const RestirDiSettings& settings,const Vec3d& color);
void temporal_mean(const std::array<Vec3d,8>& observations,const std::array<Vec3d,8>& reference,bool relative_precision=true);
RenderView temporal_view(Fixture& f,unsigned size=5,bool ortho=false,double x=.2,double angle=0,double extent=1);
RestirDiSettings spatial_settings(bool temporal=false);
PathTraceGpuImage ris_medium_image(PathTraceRenderer& r,SubmissionQueue& q,const PathTraceGpuScene& scene,const PathMediumSnapshot& m,
    VolumePathSettings s,unsigned count,const PathTraceRay& ray,const RestirDiSettings& ris={},unsigned first=0,unsigned pixel=0);
RenderView medium_down_view(Fixture& f,unsigned size=5,bool ortho=false,double height=1,double x=.2);
void medium_phase(PathTraceRenderer& r,SubmissionQueue& q,unsigned id,unsigned phase);

void restir_numeric(memory::ResourceHandle heap,PathTraceRenderer& r,SubmissionQueue& q);
void restir_materials(memory::ResourceHandle heap,PathTraceRenderer& r,SubmissionQueue& q);
void restir_textures(memory::ResourceHandle heap,PathTraceRenderer& r,SubmissionQueue& q);
void restir_schedule(memory::ResourceHandle heap,PathTraceRenderer& r,SubmissionQueue& q);
void restir_state(memory::ResourceHandle heap,PathTraceRenderer& r,SubmissionQueue& q);

void temporal_numeric(memory::ResourceHandle heap,PathTraceRenderer& r,SubmissionQueue& q);
void temporal_materials(memory::ResourceHandle heap,PathTraceRenderer& r,SubmissionQueue& q);
void temporal_schedule(memory::ResourceHandle heap,PathTraceRenderer& r,SubmissionQueue& q);
void temporal_state(memory::ResourceHandle heap,PathTraceRenderer& r,SubmissionQueue& q);
void temporal_boundaries(memory::ResourceHandle heap,PathTraceRenderer& r,SubmissionQueue& q);
void temporal_support(memory::ResourceHandle heap,PathTraceRenderer& r,SubmissionQueue& q);

void spatial_numeric(memory::ResourceHandle heap,PathTraceRenderer& r,SubmissionQueue& q);
void spatial_schedule(memory::ResourceHandle heap,PathTraceRenderer& r,SubmissionQueue& q);
void spatial_state(memory::ResourceHandle heap,PathTraceRenderer& r,SubmissionQueue& q);
void spatial_support(memory::ResourceHandle heap,PathTraceRenderer& r,SubmissionQueue& q,bool edges,bool paper=false);
void spatial_budget(memory::ResourceHandle heap,SubmissionQueue& q);

void restir_direct(memory::ResourceHandle heap,PathTraceRenderer& r,SubmissionQueue& q);

void restir_medium_numeric(memory::ResourceHandle heap,PathTraceRenderer& r,SubmissionQueue& q);
void restir_medium_lights(memory::ResourceHandle heap,PathTraceRenderer& r,SubmissionQueue& q);
void restir_medium_cloud(memory::ResourceHandle heap,PathTraceRenderer& r,SubmissionQueue& q);
void restir_medium_v2(memory::ResourceHandle heap,PathTraceRenderer& r,SubmissionQueue& q);
void restir_medium_schedule(memory::ResourceHandle heap,PathTraceRenderer& r,SubmissionQueue& q);
void restir_medium_reuse(memory::ResourceHandle heap,PathTraceRenderer& r,SubmissionQueue& q);
void restir_medium_state(memory::ResourceHandle heap,PathTraceRenderer& r,SubmissionQueue& q);
void restir_medium_failure(memory::ResourceHandle heap,SubmissionQueue& q);

void restir_medium_support(memory::ResourceHandle heap,PathTraceRenderer& r,SubmissionQueue& q,bool initial=false);
} // namespace dk::test::path_probe

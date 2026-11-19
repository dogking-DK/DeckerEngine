#pragma once
#include "PathProbeSupport.hpp"

namespace path_voxel_test { struct Fixture; }

namespace dk::test::path_probe {
RestirDiSettings voxel_ris(unsigned mode);
RenderView voxel_signal_view(const path_voxel_test::Fixture& f,bool ortho=false,double x=.5,unsigned size=5);

void voxel_numeric(memory::ResourceHandle heap,PathTraceRenderer& renderer,SubmissionQueue& queue);
void voxel_lighting(memory::ResourceHandle heap,PathTraceRenderer& r,SubmissionQueue& q);
void voxel_state(memory::ResourceHandle heap,PathTraceRenderer& r,SubmissionQueue& q);

void voxel_restir_numeric(memory::ResourceHandle heap,PathTraceRenderer& r,SubmissionQueue& q);
void voxel_restir_reuse(memory::ResourceHandle heap,PathTraceRenderer& r,SubmissionQueue& q);
void voxel_signals(memory::ResourceHandle heap,PathTraceRenderer& r,SubmissionQueue& q);
void voxel_restir_state(memory::ResourceHandle heap,PathTraceRenderer& r,SubmissionQueue& q);

void voxel_medium_numeric(memory::ResourceHandle heap,PathTraceRenderer& r,SubmissionQueue& q);
void voxel_medium_schedule(memory::ResourceHandle heap,PathTraceRenderer& r,SubmissionQueue& q);
void voxel_medium_reuse(memory::ResourceHandle heap,PathTraceRenderer& r,SubmissionQueue& q);
void voxel_medium_state(memory::ResourceHandle heap,PathTraceRenderer& r,SubmissionQueue& q);
} // namespace dk::test::path_probe

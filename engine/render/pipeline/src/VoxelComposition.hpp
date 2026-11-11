#pragma once
#include <dk/render/ScenePipeline.hpp>
#include <dk/render/VoxelGpu.hpp>
#include <dk/graphics/GraphExecution.hpp>

namespace dk::render::detail {
struct VoxelPrograms {
    graphics::ResourceFactory factory;
    graphics::PipelineLayout trace_layout,composite_layout,directional_layout,directional_depth_layout;
    graphics::ComputePipeline trace;
    graphics::GraphicsPipeline composite,directional,directional_depth;
};
[[nodiscard]] std::shared_ptr<VoxelPrograms> create_voxel_programs(memory::ResourceHandle,graphics::ResourceFactory,const std::filesystem::path&);
void validate_voxel_settings(const VoxelRenderSettings&);
struct VoxelRasterEntry {
    std::shared_ptr<const VoxelGpuGrid> owner;
    graphics::Buffer parameters,palette;
    std::size_t grid_index=0,parameters_index=0,palette_index=0,hits_index=0,status_index=0;
    graphics::graph::ResourceId grid_id,parameters_id,palette_id;
    VoxelPrograms* programs=nullptr;
    std::uint32_t count=0;
};
struct VoxelComposition {
    std::shared_ptr<VoxelPrograms> programs;
    std::vector<VoxelRasterEntry> entries;
    std::vector<std::shared_ptr<const VoxelGpuGrid>> owners;
    graphics::graph::ResourceId hits_id,status_id,status_readback_id;
    std::size_t hits_index=0,status_index=0,status_readback_index=0,first_pass=0,hdr_index=0,depth_index=0;
    std::uint32_t width=0,height=0;
    void declare_buffers(graphics::graph::Graph&,Vector<graphics::graph::ExternalBinding>&);
    void add_passes(graphics::graph::Graph&,graphics::graph::ResourceId hdr,graphics::graph::ResourceId depth,std::size_t hdr_index,std::size_t depth_index,graphics::graph::PassId after);
    void add_callbacks(Vector<graphics::graph::PassCallback>&);
};
[[nodiscard]] std::unique_ptr<VoxelComposition> prepare_voxels(graphics::SubmissionQueue&,const RenderView&,
    std::span<const VoxelInstance>,const VoxelRenderSettings&,std::shared_ptr<VoxelPrograms>);
} // namespace dk::render::detail

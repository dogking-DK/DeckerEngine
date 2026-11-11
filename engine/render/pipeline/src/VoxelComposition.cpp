#include "VoxelComposition.hpp"
#include "PipelinePolicy.hpp"
#include <dk/graphics/ShaderCompiler.hpp>
#include <dk/memory/SmartPtr.hpp>
#include <algorithm>
#include <cmath>

namespace dk::render::detail {
using namespace graphics;
namespace {
template<class T> T take(Result<T> r) { if(!r) throw r.error(); return std::move(*r); }
void check(Result<void> r) { if(!r) throw r.error(); }
Error invalid(const char* message) { return {ErrorCode::invalid_argument,message}; }
using F=std::array<float,4>;
struct RasterParameters {
    std::array<std::int32_t,4> lower{},upper{};
    std::array<std::uint32_t,4> control{};
    std::array<F,3> normals{};
    F origin{},forward{},right{},up{},projection{},light{},energy{};
    std::array<std::uint32_t,4> identity{};
};
static_assert(sizeof(RasterParameters)==224);
F pack(const Vec3d& v,double w=0) {
    if(!v.allFinite() || (v.array().abs()>std::numeric_limits<float>::max()).any() || !std::isfinite(w))
        throw invalid("Voxel raster coefficients are not finite float");
    return {float(v.x()),float(v.y()),float(v.z()),float(w)};
}
graph::Use use(graph::ResourceId id,vk::PipelineStageFlags2 stage,vk::AccessFlags2 access,bool overwrite=false) {
    return {id,{{stage,access},0,VK_WHOLE_SIZE,{vk::ImageAspectFlagBits::eColor,0,1,0,1},overwrite}};
}
Result<void> trace_pass(graph::PassContext& pass,void* pointer) {
    try {
        auto& e=*static_cast<VoxelRasterEntry*>(pointer); auto& p=*e.programs;
        const std::array writes{BindingWrite{0,0,BufferBinding{take(pass.buffer(e.parameters_index))}},BindingWrite{1,0,BufferBinding{take(pass.buffer(e.grid_index))}},
            BindingWrite{2,0,BufferBinding{take(pass.buffer(e.palette_index))}},BindingWrite{3,0,BufferBinding{take(pass.buffer(e.hits_index))}},BindingWrite{4,0,BufferBinding{take(pass.buffer(e.status_index))}}};
        auto set=take(p.factory.create_bindings(p.trace_layout,0,writes)); auto encoder=take(pass.compute());
        check(encoder.bind_pipeline(p.trace)); const std::array sets{&set}; check(encoder.bind_sets(sets));
        return encoder.dispatch({(e.count+63)/64,1,1});
    } catch(const Error& error) { return std::unexpected(error); }
}
Result<void> composite_pass(graph::PassContext& pass,void* pointer) {
    try {
        auto& c=*static_cast<VoxelComposition*>(pointer); auto& p=*c.programs;
        auto color=take(p.factory.create_view(*take(pass.image(c.hdr_index))));
        auto depth=take(p.factory.create_view(*take(pass.image(c.depth_index)),{vk::ImageViewType::e2D,{vk::ImageAspectFlagBits::eDepth,0,1,0,1}}));
        const std::array writes{BindingWrite{0,0,BufferBinding{take(pass.buffer(c.hits_index))}}};
        auto set=take(p.factory.create_bindings(p.composite_layout,0,writes));
        RenderingDesc desc; desc.color.view=&color; desc.depth.view=&depth;
        desc.color.load=desc.depth.load=vk::AttachmentLoadOp::eLoad;
        auto encoder=take(pass.begin_rendering(desc)); check(encoder.bind_pipeline(p.composite)); const std::array sets{&set}; check(encoder.bind_sets(sets));
        const std::array<std::uint32_t,4> extent{c.width,c.height,0,0};
        check(encoder.push_constants(vk::ShaderStageFlagBits::eFragment,0,std::as_bytes(std::span{extent})));
        check(encoder.draw(3)); return encoder.end();
    } catch(const Error& error) { return std::unexpected(error); }
}
}
void validate_voxel_settings(const VoxelRenderSettings& s) {
    if((s.shading!=VoxelShading::unlit && s.shading!=VoxelShading::directional) || !s.light_direction.allFinite() ||
        std::abs(s.light_direction.cast<double>().norm()-1)>1e-5 || !s.irradiance.allFinite() ||
        (s.irradiance.array()<0).any() || (s.irradiance.array()>1e6).any() || !s.payload_bytes ||
        s.payload_bytes>2ull*1024*1024*1024 || !s.maximum_regions || s.maximum_regions>1024*1024)
        throw invalid("Invalid voxel shading, direction, energy or work/payload budget");
}
std::shared_ptr<VoxelPrograms> create_voxel_programs(memory::ResourceHandle heap,ResourceFactory factory,const std::filesystem::path& directory) {
    auto p=memory::make_shared_in<VoxelPrograms>(heap); p->factory=factory;
    auto compile=[&](const char* file,const char* entry,ShaderStage stage,std::string_view define={}) {
        const std::array paths{std::filesystem::path{DK_VOXEL_SHADER_DIR},std::filesystem::path{DK_VOXEL_NANO_INCLUDE_DIR}};
        const std::array defines{ShaderDefine{define,"1"}};
        auto shader=take(compile_shader({directory/file,entry,stage,paths,define.empty() ? std::span<const ShaderDefine>{} : std::span<const ShaderDefine>{defines},true},heap));
        return take(factory.create_shader(shader));
    };
    auto cs=compile("voxel-raster.slang","computeMain",ShaderStage::compute);
    const std::array compute{&cs}; p->trace_layout=take(factory.create_pipeline_layout(compute)); p->trace=take(factory.create_compute_pipeline({&cs,&p->trace_layout}));
    auto vs=compile("voxel-composite.slang","vertexMain",ShaderStage::vertex,"DK_VERTEX");
    auto fs=compile("voxel-composite.slang","fragmentMain",ShaderStage::fragment);
    const std::array composite{&vs,&fs}; p->composite_layout=take(factory.create_pipeline_layout(composite));
    GraphicsPipelineDesc d{&vs,&fs,&p->composite_layout}; d.color_format=vk::Format::eR32G32B32A32Sfloat; d.depth_format=vk::Format::eD32Sfloat;
    d.depth_test=d.depth_write=true; d.depth_compare=vk::CompareOp::eLess; p->composite=take(factory.create_graphics_pipeline(d));
    auto dv=compile("directional.slang","vertexMain",ShaderStage::vertex,"DK_VERTEX");
    auto df=compile("directional.slang","fragmentMain",ShaderStage::fragment);
    auto dd=compile("directional.slang","depthMain",ShaderStage::fragment,"DK_DEPTH");
    const std::array lit{&dv,&df},depth{&dv,&dd};
    p->directional_layout=take(factory.create_pipeline_layout(lit)); p->directional_depth_layout=take(factory.create_pipeline_layout(depth));
    const std::array vb{vk::VertexInputBindingDescription{0,sizeof(GpuVertex),vk::VertexInputRate::eVertex}};
    const std::array va{vk::VertexInputAttributeDescription{0,0,vk::Format::eR32G32B32Sfloat,offsetof(GpuVertex,position)},
        vk::VertexInputAttributeDescription{1,0,vk::Format::eR32G32Sfloat,offsetof(GpuVertex,uv)},vk::VertexInputAttributeDescription{2,0,vk::Format::eR32G32B32Sfloat,offsetof(GpuVertex,normal)}};
    GraphicsPipelineDesc ld{&dv,&dd,&p->directional_depth_layout,vb,va}; ld.color_format=vk::Format::eUndefined; ld.depth_format=vk::Format::eD32Sfloat;
    ld.depth_test=ld.depth_write=true; p->directional_depth=take(factory.create_graphics_pipeline(ld));
    ld.fragment=&df; ld.layout=&p->directional_layout; ld.color_format=vk::Format::eR32G32B32A32Sfloat; ld.depth_write=false; ld.depth_compare=vk::CompareOp::eEqual;
    p->directional=take(factory.create_graphics_pipeline(ld)); return p;
}
std::unique_ptr<VoxelComposition> prepare_voxels(SubmissionQueue& queue,const RenderView& view,std::span<const VoxelInstance> instances,
    const VoxelRenderSettings& settings,std::shared_ptr<VoxelPrograms> programs) {
    validate_voxel_settings(settings);
    if(instances.size()>64) throw invalid("Voxel raster supports at most 64 instances");
    const auto& desc=view.description(); const auto projection=take(environment_depth(desc));
    const auto count=std::uint64_t(desc.width)*desc.height; const auto& limits=queue.device().adapter().properties.limits;
    if(!count || count>1024*1024 || count*64>limits.maxStorageBufferRange || (count+63)/64>limits.maxComputeWorkGroupCount[0])
        throw invalid("Voxel raster extent exceeds query/device limits");
    std::uint64_t payload=count*64+8;
    for(const auto& instance:instances) {
        if(instance.entity.is_nil() || !instance.grid || instance.palette.empty() || instance.palette.size()>1024*1024 ||
            instance.palette.size()<instance.grid->source().maximum_value()) throw invalid("Voxel instance has invalid identity/grid or incomplete palette");
        payload+=sizeof(RasterParameters)+instance.palette.size()*16;
        for(const auto& material:instance.palette)
            if(!material.base_color.allFinite() || (material.base_color.array()<0).any() || (material.base_color.array()>1).any() ||
                !std::isfinite(material.metallic) || material.metallic<0 || material.metallic>1 || !std::isfinite(material.roughness) || material.roughness<0 || material.roughness>1 ||
                (settings.shading==VoxelShading::directional && material.metallic!=0)) throw invalid("Voxel palette is invalid or metallic in Lambert mode");
    }
    if(payload>settings.payload_bytes) throw invalid("Voxel raster payload byte budget exceeded");
    auto result=std::make_unique<VoxelComposition>(); result->programs=std::move(programs); result->width=desc.width; result->height=desc.height;
    std::vector<const VoxelInstance*> sorted; for(const auto& instance:instances) sorted.push_back(&instance);
    std::ranges::sort(sorted,{},[](const auto* i){return i->entity;});
    auto guard=take(queue.begin());
    for(std::size_t rank=0;rank<sorted.size();++rank) {
        const auto& instance=*sorted[rank]; if(rank && instance.entity==sorted[rank-1]->entity) throw invalid("Duplicate voxel EntityId");
        check(guard.retain(instance.grid->buffer())); result->owners.push_back(instance.grid);
        const auto world=take(instance.world.compose(instance.grid->source().corner_to_local())); const auto inverse=take(world.inverse());
        const auto& domain=instance.grid->source().domain();
        std::array<bool,6> outside{true,true,true,true,true,true};
        for(unsigned corner=0;corner<8;++corner) {
            Vec4d point; point.w()=1; for(int a=0;a<3;++a) point[a]=(corner&(1u<<a)) ? double(domain.maximum[a])+1 : double(domain.minimum[a]);
            Vec4d clip=view.world_to_clip()*world.matrix()*point;
            if(!clip.allFinite()) throw invalid("Voxel bounds overflow clip space");
            outside[0]=outside[0] && clip.x() < -clip.w(); outside[1]=outside[1] && clip.x()>clip.w();
            outside[2]=outside[2] && clip.y() < -clip.w(); outside[3]=outside[3] && clip.y()>clip.w();
            outside[4]=outside[4] && clip.z()<0; outside[5]=outside[5] && clip.z()>clip.w();
        }
        if(std::ranges::any_of(outside,[](bool v){return v;})) continue;
        const auto camera=take(inverse.compose(desc.camera_world));
        RasterParameters p;
        for(int a=0;a<3;++a) { p.lower[a]=domain.minimum[a]; p.upper[a]=domain.maximum[a]; }
        p.control={static_cast<std::uint32_t>(count),static_cast<std::uint32_t>(instance.grid->buffer().size()),settings.maximum_regions,0};
        const auto normal=take(normal_matrix(world));
        for(int r=0;r<3;++r) for(int c=0;c<3;++c) p.normals[r][c]=float(normal(r,c));
        for(int c=0;c<3;++c) if(p.normals[0][c]==0 && p.normals[1][c]==0 && p.normals[2][c]==0) throw invalid("Voxel normal underflows float");
        const auto& projection_matrix=desc.projection;
        p.origin=pack(take(camera.transform_point(Vec3d::Zero())),projection.near_m);
        p.forward=pack(take(camera.transform_direction({projection_matrix(0,2)/projection_matrix(0,0),projection_matrix(1,2)/projection_matrix(1,1),-1})),projection.far_m);
        p.right=pack(take(camera.transform_direction({2/(projection_matrix(0,0)*desc.width),0,0})),projection.orthographic ? 1 : 0);
        p.up=pack(take(camera.transform_direction({0,2/(projection_matrix(1,1)*desc.height),0})),result->entries.empty() ? 1 : 0);
        if(p.origin[3]<=0 || p.forward[3]<=p.origin[3]) throw invalid("Voxel camera clipping loses float precision");
        p.projection={float(projection_matrix(2,2)),float(projection_matrix(2,3)),float(desc.width),float(desc.height)};
        p.light=pack(settings.light_direction.cast<double>()); p.energy=pack(settings.irradiance.cast<double>());
        p.identity={static_cast<std::uint32_t>(rank),static_cast<std::uint32_t>(instance.palette.size()),settings.shading==VoxelShading::directional ? 1u : 0u,0};
        VoxelRasterEntry e; e.owner=instance.grid; e.programs=result->programs.get(); e.count=static_cast<std::uint32_t>(count);
        auto& factory=result->programs->factory;
        e.parameters=take(factory.create_buffer({sizeof(p),vk::BufferUsageFlagBits::eUniformBuffer,BufferMemory::upload})); check(e.parameters.write(0,std::as_bytes(std::span{&p,1})));
        std::vector<F> colors; for(const auto& material:instance.palette) colors.push_back(pack(material.base_color.cast<double>()));
        e.palette=take(factory.create_buffer({colors.size()*sizeof(F),vk::BufferUsageFlagBits::eStorageBuffer,BufferMemory::upload}));
        check(e.palette.write(0,std::as_bytes(std::span{colors}))); result->entries.push_back(std::move(e));
    }
    return result;
}
void VoxelComposition::declare_buffers(graph::Graph& graph,Vector<graph::ExternalBinding>& external) {
    if(entries.empty()) return;
    hits_index=graph.counts().buffers; hits_id=take(graph.declare_buffer("voxel nearest surfaces",{std::uint64_t(width)*height*64,vk::BufferUsageFlagBits::eStorageBuffer}));
    status_index=graph.counts().buffers; status_id=take(graph.declare_buffer("voxel query status",{4,vk::BufferUsageFlagBits::eStorageBuffer|vk::BufferUsageFlagBits::eTransferDst|vk::BufferUsageFlagBits::eTransferSrc}));
    status_readback_index=graph.counts().buffers; status_readback_id=take(graph.declare_buffer("voxel status readback",{4,vk::BufferUsageFlagBits::eTransferDst,BufferMemory::readback}));
    for(std::size_t i=0;i<entries.size();++i) {
        auto& e=entries[i]; e.hits_index=hits_index; e.status_index=status_index;
        std::size_t previous=0; while(previous<i && entries[previous].owner->buffer().handle()!=e.owner->buffer().handle()) ++previous;
        if(previous<i) { e.grid_id=entries[previous].grid_id; e.grid_index=entries[previous].grid_index; }
        else { e.grid_index=graph.counts().buffers; e.grid_id=take(graph.declare_buffer("voxel grid "+std::to_string(i),e.owner->buffer().description(),graph::Lifetime::external,true)); external.push_back({e.grid_index,&e.owner->buffer()}); }
        e.parameters_index=graph.counts().buffers; e.parameters_id=take(graph.declare_buffer("voxel camera "+std::to_string(i),e.parameters.description(),graph::Lifetime::external,true)); external.push_back({e.parameters_index,&e.parameters});
        e.palette_index=graph.counts().buffers; e.palette_id=take(graph.declare_buffer("voxel palette "+std::to_string(i),e.palette.description(),graph::Lifetime::external,true)); external.push_back({e.palette_index,&e.palette});
    }
}
void VoxelComposition::add_passes(graph::Graph& graph,graph::ResourceId hdr,graph::ResourceId depth,std::size_t hi,std::size_t di,graph::PassId after) {
    if(entries.empty()) return; first_pass=graph.counts().passes; hdr_index=hi; depth_index=di;
    constexpr auto compute=vk::PipelineStageFlagBits2::eComputeShader;
    for(std::size_t i=0;i<entries.size();++i) {
        const auto& e=entries[i];
        const std::array uses{use(e.grid_id,compute,vk::AccessFlagBits2::eShaderStorageRead),use(e.parameters_id,compute,vk::AccessFlagBits2::eUniformRead),
            use(e.palette_id,compute,vk::AccessFlagBits2::eShaderStorageRead),use(hits_id,compute,i==0 ? vk::AccessFlags2{vk::AccessFlagBits2::eShaderStorageWrite} : vk::AccessFlagBits2::eShaderStorageRead|vk::AccessFlagBits2::eShaderStorageWrite,i==0),
            use(status_id,compute,vk::AccessFlagBits2::eShaderStorageRead|vk::AccessFlagBits2::eShaderStorageWrite)};
        auto pass=take(graph.add_pass({"voxel HDDA "+std::to_string(i),uses})); check(graph.add_dependency(after,pass)); after=pass;
    }
    auto color=use(hdr,vk::PipelineStageFlagBits2::eColorAttachmentOutput,vk::AccessFlagBits2::eColorAttachmentRead|vk::AccessFlagBits2::eColorAttachmentWrite);
    color.access.state.layout=vk::ImageLayout::eColorAttachmentOptimal;
    auto z=use(depth,vk::PipelineStageFlagBits2::eEarlyFragmentTests|vk::PipelineStageFlagBits2::eLateFragmentTests,vk::AccessFlagBits2::eDepthStencilAttachmentRead|vk::AccessFlagBits2::eDepthStencilAttachmentWrite);
    z.access.state.layout=vk::ImageLayout::eDepthStencilAttachmentOptimal; z.access.range.aspectMask=vk::ImageAspectFlagBits::eDepth;
    const std::array uses{use(hits_id,vk::PipelineStageFlagBits2::eFragmentShader,vk::AccessFlagBits2::eShaderStorageRead),color,z};
    static_cast<void>(take(graph.add_pass({"voxel depth HDR composite",uses})));
}
void VoxelComposition::add_callbacks(Vector<graph::PassCallback>& callbacks) {
    if(entries.empty()) return;
    for(std::size_t i=0;i<entries.size();++i) callbacks.push_back({first_pass+i,trace_pass,&entries[i]});
    callbacks.push_back({first_pass+entries.size(),composite_pass,this});
}
} // namespace dk::render::detail

#include <dk/render/PathTraceRenderer.hpp>
#include <dk/graphics/GraphExecution.hpp>
#include <dk/graphics/ShaderCompiler.hpp>
#include "PathReferenceInternal.hpp"
#include <Eigen/LU>
#include <algorithm>
#include <bit>
#include <cmath>

namespace dk::render {
using namespace graphics;
namespace detail {
using Float4=std::array<float,4>;
struct PathGpuPixel { Float4 mean,second,hit,normal; };
static_assert(sizeof(PathGpuPixel)==64);
struct PathGpuView;
struct PathGpuBatch;
struct PathGpuGeometry;
struct PathGpuCloud { CloudFieldKey key; std::uint64_t hash=0; Buffer data; };
struct PathGpuState {
    memory::ResourceHandle heap;
    ResourceFactory factory;
    Buffer identity;
    PipelineLayout layout,empty_layout;
    RayTracingPipeline pipeline;
    ShaderBindingTable table;
    PipelineLayout restir_layout,restir_empty_layout;
    RayTracingPipeline restir_pipeline;
    ShaderBindingTable restir_table;
    ComputePipeline restir_empty_pipeline;
    ComputePipeline empty_pipeline,tone_pipeline;
    ComputePipeline air_empty_pipeline;
    PipelineLayout air_empty_layout;
    PipelineLayout tone_layout;
    bool textured = false;
    std::uint32_t texture_capacity=64;
    std::filesystem::path shader_directory;
    std::shared_ptr<AllocationBudget> budget;
    std::array<std::shared_ptr<PathGpuView>,2> views;
    std::shared_ptr<PathGpuBatch> active;
    std::uint64_t generation = 0;
    bool terminal = false;
    std::array<std::weak_ptr<PathGpuGeometry>,2> geometry_cache;
    std::shared_ptr<const PathGpuCloud> cloud;
};
struct PathGpuTexture { Image image; ImageView view; Sampler sampler; };
struct PathGpuGeometry {
    PathTraceScene source;
    Buffer triangles;
    AccelerationStructure blas,tlas;
    Submission built;
    explicit PathGpuGeometry(memory::ResourceHandle heap) : textures(memory::Allocator<PathGpuTexture>{heap}) {}
    Vector<PathGpuTexture> textures;
    std::size_t texture_count = 0;
};
struct PathGpuScene {
    std::weak_ptr<PathGpuState> program;
    PathTraceScene source;
    std::shared_ptr<PathGpuGeometry> geometry;
};
struct PathGpuFrame {
    memory::ResourceHandle heap;
    PathTraceScene source;
    PathTraceGpuImageInfo info;
    Buffer readback;
    Submission ticket;
    std::size_t stride = 64;
    std::uint64_t diagnostic_offset=0;
    vk::DeviceSize diagnostic_stride=0,diagnostic_bytes=0;
    std::uint32_t tile_count=1;
};
struct PathGpuCompleted {
    memory::ResourceHandle heap;
    ResourceFactory factory;
    PathTraceScene source;
    PathTraceGpuImageInfo info;
    Buffer buffer;
    Buffer surface,reservoir; // Last complete RIS epoch; immutable with this owner.
};
struct PathGpuView {
    explicit PathGpuView(RenderView v) : view(std::move(v)) {}
    std::uint64_t id=0,generation=0;
    std::shared_ptr<const PathGpuScene> scene;
    RenderView view;
    PathTraceSettings settings;
    std::optional<RestirDiSettings> restir;
    std::optional<PathMediumSnapshot> medium;
    std::shared_ptr<const PathGpuCloud> cloud;
    VolumePathSettings medium_settings;
    PathTraceSchedule schedule;
    std::shared_ptr<const PathGpuCompleted> completed;
    std::shared_ptr<const PathGpuCompleted> sampling_history;
    std::uint32_t epoch_origin=0;
};
struct PathGpuBatch {
    std::shared_ptr<PathGpuView> owner;
    std::uint64_t generation=0;
    std::shared_ptr<PathGpuCompleted> candidate;
    std::shared_ptr<const PathGpuCompleted> history;
    std::shared_ptr<const PathGpuCompleted> sampling_history;
    std::shared_ptr<const PathGpuFrame> frame;
    std::uint32_t first=0,count=0,edge=0,next_tile=0,columns=0,rows=0;
    std::uint32_t epoch=0,phase=0;
    std::uint32_t tiles_per_submission=1;
    Buffer scratch,scratch_surface,scratch_reservoir,spatial_output;
    std::stop_token stop, tile_stop;
    bool discarded=false,reverse_tiles=false;
};
struct PathGpuImage {
    explicit PathGpuImage(memory::ResourceHandle heap) : colors(0,memory::Allocator<Vec3d>{heap}),
        moments(0,memory::Allocator<Vec3d>{heap}),hits(0,memory::Allocator<std::optional<PathTraceHit>>{heap}) {}
    PathTraceGpuImageInfo info;
    Vector<Vec3d> colors,moments;
    Vector<std::optional<PathTraceHit>> hits;
};
}
namespace {
using namespace detail;
constexpr std::size_t path_status_words=16;
constexpr std::size_t restir_status_shards=256;
constexpr std::size_t restir_status_words=path_status_words*restir_status_shards;
constexpr std::size_t restir_status_bytes=restir_status_words*sizeof(std::uint32_t);
template<class T> T take(Result<T> result) { if (!result) throw result.error(); return std::move(*result); }
void check(Result<void> result) { if (!result) throw result.error(); }
Error sample_error(std::uint32_t diagnostic) {
    if(diagnostic&2) return {ErrorCode::invalid_argument,"GPU medium tracking budget exhausted",{"render.pathtracing.tracking_budget"}};
    if(diagnostic&4) return {ErrorCode::invalid_argument,"GPU medium path exceeds numerical domain",{"render.pathtracing.medium_domain"}};
    if(diagnostic&8) return {ErrorCode::internal_error,"GPU medium extinction exceeds majorant"};
    return {ErrorCode::internal_error,"Invalid GPU path sample/history"};
}
void append_restir_statistics(PathTraceGpuImageInfo& info,std::span<const std::uint32_t> diagnostic) {
    auto& temporal=info.temporal_statistics;
    auto& spatial=info.spatial_statistics;
    auto& medium=info.restir_medium_statistics;
    // Tile work caps bound every uint32 shard. SUM fields widen before their
    // first addition; MAX fields retain the original integer semantics.
    for(std::size_t shard=0;shard<restir_status_shards;++shard) {
        const auto words=diagnostic.subspan(shard*path_status_words,path_status_words);
        temporal.reused+=std::uint64_t{words[2]}; temporal.rejected+=std::uint64_t{words[3]}; temporal.expired+=std::uint64_t{words[4]};
        temporal.max_age=std::max(temporal.max_age,words[5]); temporal.max_candidates=std::max(temporal.max_candidates,words[6]);
        temporal.support_corrected+=std::uint64_t{words[7]};
        spatial.reused+=std::uint64_t{words[8]}; spatial.rejected+=std::uint64_t{words[9]}; spatial.support_corrected+=std::uint64_t{words[10]};
        spatial.max_candidates=std::max(spatial.max_candidates,words[11]); spatial.max_age=std::max(spatial.max_age,words[12]);
        spatial.current_visibility_rays+=std::uint64_t{words[13]};
        medium.prepared_surfaces+=std::uint64_t{words[14]}; medium.applied_surfaces+=std::uint64_t{words[15]};
    }
}
void cancelled(std::stop_token stop) {
    if (stop.stop_requested()) throw Error{ErrorCode::invalid_state,"GPU path tracing cancelled",{"render.pathtracing.cancelled"}};
}
void ready(const std::shared_ptr<PathGpuState>& state,SubmissionQueue& queue,std::stop_token stop) {
    cancelled(stop);
    if (!state || state->heap.state()!=memory::ResourceState::open) throw Error{ErrorCode::invalid_state,"Path renderer requires open Memory"};
    auto guard=take(queue.begin()); check(guard.retain(state->identity));
}
Float4 pack(const Vec3d& v,double w=0) {
    Float4 value{float(v.x()),float(v.y()),float(v.z()),float(w)};
    for (const auto component:value) if (!std::isfinite(component)) throw path_invalid("Path parameter exceeds finite float domain");
    return value;
}
Float4 integers(std::uint32_t x,std::uint32_t y,std::uint32_t z,std::uint32_t w) {
    return {std::bit_cast<float>(x),std::bit_cast<float>(y),std::bit_cast<float>(z),std::bit_cast<float>(w)};
}
Buffer upload(const ResourceFactory& factory,std::span<const std::byte> bytes,vk::BufferUsageFlags usage) {
    auto buffer=take(factory.create_buffer({bytes.size(),usage,BufferMemory::upload})); check(buffer.write(0,bytes)); return buffer;
}
graph::Use use(graph::BufferId id,vk::PipelineStageFlags2 stage,vk::AccessFlags2 access,bool write=false) {
    return {id,{{stage,access},0,VK_WHOLE_SIZE,{},write}};
}
graph::PassId add(graph::Graph& graph,std::string_view name,std::span<const graph::Use> uses) { return take(graph.add_pass({name,uses})); }
graph::AccelerationId declare(graph::Graph& graph,std::string_view name,AccelerationStructureKind kind,
    std::span<const graph::ResourceId> deps,bool built=false) { return take(graph.declare_acceleration(name,kind,deps,built)); }
struct BuildContext { vk::DeviceSize bytes; const PathGpuScene& scene; };
Result<void> build_record(graph::PassContext& pass,void* context) {
    const auto& c=*static_cast<BuildContext*>(context); const auto count=c.scene.geometry->texture_count;
    if (pass.pass_index()==0) {
        if(auto copied=pass.copy_buffer(0,1,c.bytes);!copied) return copied;
        for(std::size_t i=0;i<count;++i) {
            const auto d=c.scene.geometry->textures[i].image.description();
            if(auto copied=pass.copy_to_image(2+i,2+count+i,{0,0,0,0,d.width,d.height});!copied) return copied;
        }
        return {};
    }
    return pass.build(2*count+pass.pass_index()+1);
}
using Parameters=std::array<Float4,42>;
struct TraceContext {
    const PathGpuState& state; std::span<const BindingSet> bindings;
    std::span<const Parameters> parameters;
    std::uint32_t width,height; bool empty;
    std::size_t shader_table;
    bool progressive=false;
    bool air=false;
    bool restir=false;
    vk::DeviceSize diagnostic_bytes=0;
    std::stop_token stop;
    std::stop_token batch_stop;
};
Result<void> trace_record(graph::PassContext& pass,void* context) {
    const auto& c=*static_cast<TraceContext*>(context);
    try {
        if (pass.pass_index()==0) return pass.fill(6);
        if (pass.pass_index()>c.bindings.size()) {
            if(c.progressive) return pass.copy_buffer(6,4,c.diagnostic_bytes);
            const auto bytes=std::uint64_t{c.width}*c.height*sizeof(PathGpuPixel);
            return pass.pass_index()==c.bindings.size()+1 ? pass.copy_buffer(3,4,bytes) : pass.copy_buffer(6,4,4,0,bytes);
        }
        cancelled(c.stop); cancelled(c.batch_stop);
        const auto index=pass.pass_index()-1;
        const auto width=std::bit_cast<std::uint32_t>(c.parameters[index][0][0]);
        const auto height=std::bit_cast<std::uint32_t>(c.parameters[index][0][1]);
        const std::array<const BindingSet*,1> sets{&c.bindings[index]};
        if (!c.empty) return pass.trace(c.restir ? c.state.restir_pipeline : c.state.pipeline,c.shader_table,sets,{}, {width,height,1});
        auto encoder=take(pass.compute()); check(encoder.bind_pipeline(c.restir ? c.state.restir_empty_pipeline : (c.air ? c.state.air_empty_pipeline : c.state.empty_pipeline))); check(encoder.bind_sets(sets));
        return encoder.dispatch({(width*height+63)/64,1,1});
    } catch (const Error& error) { return std::unexpected(error); }
}
void restir_memory(const PathGpuState&,std::uint64_t,std::uint32_t,const RestirDiSettings&);
std::uint64_t restir_evaluations(const RestirDiSettings& r) {
    return r.initial_candidates+(r.temporal ? 2 : 0)+(r.spatial ? 2*r.spatial->neighbor_count+1 : 0);
}
Parameters view_parameters(const PathSceneState&,const RenderView&,const PathTraceSettings&,PathTraceSamples);
Parameters parameters(const PathSceneState& scene,const PathTraceSettings& s,PathTraceSamples samples,
    std::uint32_t width,std::uint32_t height,bool rays,std::uint32_t first_pixel) {
    const auto env=scene.source.environment();
    const Vec3d sun{env.sun.irradiance[0],env.sun.irradiance[1],env.sun.irradiance[2]};
    const Vec3d direction{env.sun.direction[0],env.sun.direction[1],env.sun.direction[2]};
    const bool solar=s.sun==PathTraceSun::scene && sun.maxCoeff()>0;
    const auto classes=int(s.environment_rgb.maxCoeff()>0)+int(solar)+int(scene.light_weight>0);
    Parameters p{}; p[0]=integers(width,height,samples.first,samples.count);
    p[1]=integers(std::uint32_t(s.seed),std::uint32_t(s.seed>>32),s.max_bounces,s.rr_start);
    p[2]=integers(s.estimator==PathTraceEstimator::mis ? 0u : 1u,solar ? 1u : 0u,std::uint32_t(scene.triangles.size()),rays ? 1u : 0u);
    p[3]=pack(s.environment_rgb,scene.light_weight); p[4]=pack(sun,classes ? 1.0/classes : 0);
    p[5]=pack(direction); p[10]=integers(first_pixel,0,0,0); return p;
}
std::shared_ptr<const PathGpuFrame> submit(PathGpuState& state,SubmissionQueue& queue,const PathGpuScene& scene,
    Parameters p,std::span<const Float4> rays,PathTraceGpuImageInfo info,std::stop_token stop,const Buffer* candidate=nullptr,const Buffer* completed=nullptr,std::shared_ptr<const PathGpuCloud> cloud={},
    const Buffer* surface=nullptr,const Buffer* reservoir=nullptr,const Buffer* old_surface=nullptr,const Buffer* old_reservoir=nullptr,
    const Buffer* spatial_input=nullptr,std::span<const Parameters> grouped={},std::stop_token batch_stop={}) {
    const bool empty=scene.source.triangle_count()==0;
    const bool air=info.medium.has_value();
    const bool restir=info.restir.has_value();
    if(restir) p[32][0]=std::bit_cast<float>(info.restir->initial_candidates);
    const auto dispatches=grouped.empty() ? std::span<const Parameters>{&p,1} : grouped;
    const auto alignment=std::max(vk::DeviceSize{1},queue.device().adapter().properties.limits.minStorageBufferOffsetAlignment);
    const auto aligned=[&](vk::DeviceSize n) { return ((n+alignment-1)/alignment)*alignment; };
    const auto parameter_stride=aligned(sizeof(Parameters));
    const auto count=std::uint64_t{info.width}*info.height,bytes=count*sizeof(PathGpuPixel);
    auto frame=memory::make_shared_in<PathGpuFrame>(state.heap); frame->heap=state.heap; frame->source=scene.source; frame->info=std::move(info);
    const auto storage=vk::BufferUsageFlagBits::eStorageBuffer;
    Buffer dummy_surface,dummy_reservoir;
    if(restir && !surface) { dummy_surface=take(state.factory.create_buffer({64,storage})); surface=&dummy_surface; }
    if(restir && !reservoir) { dummy_reservoir=take(state.factory.create_buffer({32,storage})); reservoir=&dummy_reservoir; }
    Vector<std::byte> parameter_bytes(parameter_stride*dispatches.size(),memory::Allocator<std::byte>{state.heap});
    for(std::size_t i=0;i<dispatches.size();++i) {
        const auto data=std::as_bytes(std::span{dispatches[i]});
        std::copy(data.begin(),data.end(),parameter_bytes.begin()+i*parameter_stride);
    }
    auto uniform=upload(state.factory,parameter_bytes,storage);
    auto input=upload(state.factory,std::as_bytes(rays),storage);
    Buffer allocated;
    if(!candidate) allocated=take(state.factory.create_buffer({bytes,storage|vk::BufferUsageFlagBits::eTransferSrc}));
    const auto& output=candidate ? *candidate : allocated;
    const std::array<Float4,8> zero{};
    Buffer initial;
    if(!completed) initial=upload(state.factory,std::as_bytes(std::span{zero}),storage);
    const auto& history=completed ? *completed : initial;
    // Unused previous planes bind a real, initialized buffer and are never indexed.
    if(!old_surface) old_surface=&uniform;
    if(!old_reservoir) old_reservoir=&uniform;
    const bool spatial_pass=restir && std::bit_cast<std::uint32_t>(p[32][1])==4;
    if(!spatial_input) spatial_input=&uniform;
    // Errors and medium tracking remain global words 0/1. Observational RIS
    // counters occupy 256 shards, initialized on the device in this submission.
    const auto status_bytes=restir ? restir_status_bytes : path_status_words*sizeof(std::uint32_t);
    frame->tile_count=std::uint32_t(dispatches.size()); frame->diagnostic_stride=aligned(status_bytes);
    frame->diagnostic_bytes=(dispatches.size()-1)*frame->diagnostic_stride+(restir && candidate ? restir_status_bytes : 4ull);
    auto status=take(state.factory.create_buffer({dispatches.size()*frame->diagnostic_stride,storage|vk::BufferUsageFlagBits::eTransferSrc|vk::BufferUsageFlagBits::eTransferDst}));
    frame->readback=take(state.factory.create_buffer({candidate ? frame->diagnostic_bytes : bytes+4,vk::BufferUsageFlagBits::eTransferDst,BufferMemory::readback}));
    frame->diagnostic_offset=candidate ? 0 : bytes;
    Vector<BindingWrite> writes(0,memory::Allocator<BindingWrite>{state.heap});
    if (!empty) { writes.push_back({0,0,AccelerationStructureBinding{&scene.geometry->tlas}}); writes.push_back({1,0,BufferBinding{&scene.geometry->triangles}}); }
    if(!empty || air || restir) { writes.push_back({4,0,BufferBinding{&input}});
        writes.push_back({9,0,BufferBinding{cloud ? &cloud->data : &uniform}}); }
    const auto parameter_binding=writes.size(); writes.push_back({2,0,BufferBinding{&uniform,0,sizeof(Parameters)}}); writes.push_back({3,0,BufferBinding{&output}});
    writes.push_back({7,0,BufferBinding{&history}}); const auto diagnostic_binding=writes.size(); writes.push_back({8,0,BufferBinding{&status,0,status_bytes}});
    if(restir) {
        writes.push_back({10,0,BufferBinding{surface}}); writes.push_back({11,0,BufferBinding{reservoir}});
        writes.push_back({12,0,BufferBinding{old_surface}}); writes.push_back({13,0,BufferBinding{old_reservoir}});
        writes.push_back({14,0,BufferBinding{spatial_input}});
    }
    if(!empty && state.textured) for(std::uint32_t i=0;i<state.texture_capacity;++i) {
        const auto& texture=scene.geometry->textures[std::min<std::size_t>(i,scene.geometry->texture_count-1)];
        writes.push_back({5,i,ImageBinding{&texture.view}}); writes.push_back({6,i,SamplerBinding{&texture.sampler}});
    }
    const auto& layout=restir ? (empty ? state.restir_empty_layout : state.restir_layout) : (empty ? (air ? state.air_empty_layout : state.empty_layout) : state.layout);
    Vector<BindingSet> bindings(0,memory::Allocator<BindingSet>{state.heap}); bindings.reserve(dispatches.size());
    for(std::size_t i=0;i<dispatches.size();++i) {
        cancelled(stop);
        writes[parameter_binding].resource=BufferBinding{&uniform,i*parameter_stride,sizeof(Parameters)};
        writes[diagnostic_binding].resource=BufferBinding{&status,i*frame->diagnostic_stride,status_bytes};
        bindings.push_back(take(state.factory.create_bindings(layout,0,writes)));
    }
    auto g=take(graph::Graph::create(state.heap));
    const auto params=take(g.declare_buffer("parameters",uniform.description(),graph::Lifetime::external,true));
    const auto geometry=take(g.declare_buffer("triangles",empty ? BufferDesc{4,storage} : scene.geometry->triangles.description(),graph::Lifetime::external,true));
    const auto ray_input=take(g.declare_buffer("rays",input.description(),graph::Lifetime::external,true));
    const auto result=take(g.declare_buffer("HDR and diagnostics",output.description(),graph::Lifetime::external));
    const auto readback=take(g.declare_buffer("readback",frame->readback.description(),graph::Lifetime::external));
    const auto previous=take(g.declare_buffer("completed history",history.description(),graph::Lifetime::external,true));
    const auto diagnostic=take(g.declare_buffer("tile status",status.description(),graph::Lifetime::external));
    const auto stage=empty ? vk::PipelineStageFlagBits2::eComputeShader : vk::PipelineStageFlagBits2::eRayTracingShaderKHR;
    Vector<graph::Use> uses(0,memory::Allocator<graph::Use>{state.heap});
    uses.push_back(use(params,stage,vk::AccessFlagBits2::eShaderRead)); uses.push_back(use(result,stage,vk::AccessFlagBits2::eShaderWrite,true));
    uses.push_back(use(previous,stage,vk::AccessFlagBits2::eShaderRead));
    uses.push_back(use(diagnostic,stage,vk::AccessFlagBits2::eShaderRead|vk::AccessFlagBits2::eShaderWrite));
    Vector<graph::ExternalBinding> imports(0,memory::Allocator<graph::ExternalBinding>{state.heap});
    imports.push_back({0,&uniform}); imports.push_back({3,&output}); imports.push_back({4,&frame->readback}); imports.push_back({5,&history}); imports.push_back({6,&status});
    if(restir) {
        const auto phase=std::bit_cast<std::uint32_t>(p[32][1]);
        const bool reading=phase==1 || phase==3 || spatial_pass;
        const auto access=reading ? vk::AccessFlagBits2::eShaderRead : vk::AccessFlagBits2::eShaderWrite;
        const auto a=take(g.declare_buffer("RIS surface",surface->description(),graph::Lifetime::external,reading));
        const auto b=take(g.declare_buffer("RIS reservoir",reservoir->description(),graph::Lifetime::external,reading && !spatial_pass));
        // As with the candidate image, tile writes are discard writes: no previous contents
        // are consumed. The scheduler gates every reader on completion of the whole plane.
        uses.push_back(use(a,stage,access,!reading));
        uses.push_back(use(b,stage,spatial_pass ? vk::AccessFlagBits2::eShaderWrite : (phase==3 ? access|vk::AccessFlagBits2::eShaderWrite : access),!reading || spatial_pass));
        imports.push_back({7,surface}); imports.push_back({8,reservoir});
        // Only declare previous planes when they are used: dummy bindings alias parameters.
        if(std::bit_cast<std::uint32_t>(p[33][2])!=0) {
            auto old_a=take(g.declare_buffer("previous RIS surface",old_surface->description(),graph::Lifetime::external,true));
            auto old_b=take(g.declare_buffer("previous RIS reservoir",old_reservoir->description(),graph::Lifetime::external,true));
            uses.push_back(use(old_a,stage,vk::AccessFlagBits2::eShaderRead)); uses.push_back(use(old_b,stage,vk::AccessFlagBits2::eShaderRead));
            imports.push_back({9,old_surface}); imports.push_back({10,old_reservoir});
        }
        if(spatial_pass) {
            const auto input_plane=take(g.declare_buffer("frozen spatial RIS input",spatial_input->description(),graph::Lifetime::external,true));
            uses.push_back(use(input_plane,stage,vk::AccessFlagBits2::eShaderRead));
            imports.push_back({std::bit_cast<std::uint32_t>(p[33][2])!=0 ? 11u : 9u,spatial_input});
        }
    }
    const auto restir_buffers=restir ? (std::bit_cast<std::uint32_t>(p[33][2])!=0 ? 4u : 2u)+(spatial_pass ? 1u : 0u) : 0u;
    if(empty && (air || restir)) { uses.push_back(use(ray_input,stage,vk::AccessFlagBits2::eShaderRead)); imports.push_back({2,&input}); }
    // Compiled Graph resources are grouped by type: buffers (including clouds), images, AS, SBT.
    for(std::size_t i=0;i<scene.geometry->texture_count;++i) {
        const auto& texture=scene.geometry->textures[i];
        auto id=take(g.declare_image("baseColor-"+std::to_string(i),texture.image.description(),graph::Lifetime::external,true));
        uses.push_back({id,{{stage,vk::AccessFlagBits2::eShaderSampledRead,vk::ImageLayout::eShaderReadOnlyOptimal}}});
        imports.push_back({7+i+(cloud ? 1u : 0u)+restir_buffers,nullptr,&texture.image});
    }
    if (!empty) {
        const auto blas=declare(g,"BLAS",AccelerationStructureKind::bottom_level,{},true);
        const std::array<graph::ResourceId,1> children{blas}; const auto tlas=declare(g,"TLAS",AccelerationStructureKind::top_level,children,true);
        const auto sbt=take(g.declare_shader_table("SBT"));
        uses.push_back(use(geometry,stage,vk::AccessFlagBits2::eShaderRead)); uses.push_back(use(ray_input,stage,vk::AccessFlagBits2::eShaderRead));
        uses.push_back(graph::acceleration_read(blas)); uses.push_back(graph::acceleration_read(tlas)); uses.push_back(graph::shader_table_read(sbt));
        imports.push_back({1,&scene.geometry->triangles}); imports.push_back({2,&input});
        const auto base=7+scene.geometry->texture_count+(cloud ? 1u : 0u)+restir_buffers;
        imports.push_back({base,nullptr,nullptr,{},&scene.geometry->blas}); imports.push_back({base+1,nullptr,nullptr,{},&scene.geometry->tlas}); imports.push_back({base+2,nullptr,nullptr,{},nullptr,restir ? &state.restir_table : &state.table});
    }
    if(cloud) {
        const auto id=take(g.declare_buffer("immutable cloud fields",cloud->data.description(),graph::Lifetime::external,true));
        uses.push_back(use(id,stage,vk::AccessFlagBits2::eShaderRead)); imports.push_back({7+restir_buffers,&cloud->data});
    }
    const std::array clear{use(diagnostic,vk::PipelineStageFlagBits2::eClear,vk::AccessFlagBits2::eTransferWrite,true)};
    add(g,"clear path status",clear);
    for(std::size_t i=0;i<dispatches.size();++i) {
        const auto name=std::string(empty ? "empty environment" : "surface path trace")+
            (dispatches.size()>1 ? " tile "+std::to_string(i) : "");
        add(g,name,uses);
    }
    Vector<graph::Use> copy(0,memory::Allocator<graph::Use>{state.heap});
    copy.push_back(use(candidate ? diagnostic : result,vk::PipelineStageFlagBits2::eCopy,vk::AccessFlagBits2::eTransferRead));
    copy.push_back(use(readback,vk::PipelineStageFlagBits2::eCopy,vk::AccessFlagBits2::eTransferWrite,true));
    add(g,"HDR readback",copy);
    if(!candidate) {
        copy[0]=use(diagnostic,vk::PipelineStageFlagBits2::eCopy,vk::AccessFlagBits2::eTransferRead);
        copy[1]=use(readback,vk::PipelineStageFlagBits2::eCopy,vk::AccessFlagBits2::eTransferWrite);
        add(g,"path diagnostic readback",copy);
    }
    check(g.mark_output(readback)); check(g.mark_output(result));
    auto plan=take(g.compile(stop)); TraceContext context{state,bindings,dispatches,frame->info.width,frame->info.height,empty,9+scene.geometry->texture_count+(cloud ? 1u : 0u)+restir_buffers,candidate!=nullptr,air,restir,frame->diagnostic_bytes,stop,batch_stop};
    Vector<graph::PassCallback> callbacks(0,memory::Allocator<graph::PassCallback>{state.heap});
    for(std::size_t i=0;i<dispatches.size()+(candidate ? 2 : 3);++i) callbacks.push_back({i,trace_record,&context});
    const std::array final{graph::FinalAccess{4,{{vk::PipelineStageFlagBits2::eHost,vk::AccessFlagBits2::eHostRead}}}};
    cancelled(stop); auto execution=take(graph::execute(plan,queue,{imports,callbacks,final}));
    frame->ticket=execution.submission(); if(cloud) state.cloud=std::move(cloud); return frame; // No allocation/fallible work after successful submit.
}
PathTraceGpuImageInfo info_for(const PathTraceScene& scene,std::uint32_t width,std::uint32_t height,
    const PathTraceSettings& settings,PathTraceSamples samples) {
    PathTraceGpuImageInfo info; info.width=width; info.height=height; info.scene=scene.id(); info.revision=scene.revision();
    info.fingerprint=scene.fingerprint(); info.geometry_fingerprint=scene.geometry_fingerprint(); info.texture_count=scene.texture_count(); info.masked_emitter_count=scene.masked_emitter_count();
    info.material_model=scene.material_model(); info.settings=settings; info.samples=samples; return info;
}
PathTraceSettings common_settings(const VolumePathSettings& s) {
    PathTraceSettings r; r.model_version=s.model_version; r.sampler_version=s.sampler_version; r.seed=s.seed;
    r.max_bounces=s.max_bounces; r.rr_start=s.rr_start; r.environment_rgb=s.environment_rgb;
    r.sun=s.sun ? PathTraceSun::scene : PathTraceSun::none; r.estimator=s.estimator;
    r.max_rays=s.max_rays; r.max_triangle_tests=s.max_triangle_tests; return r;
}
void validate_medium(const PathTraceScene& scene,const PathMediumSnapshot& m,const VolumePathSettings& s,PathTraceSamples samples,std::uint64_t pixels) {
    check(validate_volume_work(scene,m,s,samples,pixels,false));
    const auto& c=m.environment().clouds;
    if(m.field() && (m.environment().atmosphere.ground_radius_m<1000 || c.weather_scale_m<100 || c.shape_scale_m<100 || c.detail_scale_m<10))
        throw Error{ErrorCode::not_supported,"GPU cloud radius/noise scale outside supported domain"};
    if(m.field() && (!(float(c.bottom_height_m)<float(c.top_height_m)) || float(c.coverage)==0 || float(c.density)*float(c.extinction_per_m)==0))
        throw Error{ErrorCode::not_supported,"GPU cloud shell or positive coefficient collapses in float"};
}
Float4 radial_origin(const PathMediumSnapshot& m,const Vec3d& origin) {
    if(!m.environment().enabled && !m.environment().clouds.enabled) return {0,1,0,0};
    const Vec3d quantized=origin.cast<float>().cast<double>(); (void)take(m.sample(quantized));
    const auto& a=m.environment().atmosphere;
    const Vec3d delta=quantized-Vec3d{a.planet_center_m[0],a.planet_center_m[1],a.planet_center_m[2]};
    return pack(delta.normalized(),delta.stableNorm()-a.ground_radius_m);
}
void medium_parameters(Parameters& p,const PathMediumSnapshot& medium,const VolumePathSettings& s) {
    const auto& e=medium.environment(); const auto& a=e.atmosphere;
    const auto rgb=[](const EnvironmentVector& v) { return Vec3d{v[0],v[1],v[2]}; };
    p[12]=integers(1,(e.enabled || e.clouds.enabled) ? 1u : 0u,s.max_steps_per_path,std::uint32_t(s.max_tracking_steps));
    p[14]={float(a.ground_radius_m),float(a.atmosphere_height_m),0,0};
    p[15]=pack(rgb(a.rayleigh_scattering_per_m),a.rayleigh_scale_height_m);
    p[16]=pack(rgb(a.mie_scattering_per_m),a.mie_scale_height_m);
    p[17]=pack(rgb(a.mie_extinction_per_m),a.mie_anisotropy);
    p[18]=pack(rgb(a.absorption_extinction_per_m),a.absorption_center_height_m);
    p[19]=pack(rgb(a.ground_albedo),a.absorption_half_width_m);
    const auto conservative=[](const Vec3d& value) {
        auto result=pack(value*(1+8*std::numeric_limits<float>::epsilon()));
        for(unsigned c=0;c<3;++c) if(result[c]>0) result[c]=std::nextafter(result[c],std::numeric_limits<float>::infinity());
        return result;
    };
    auto air=e; air.clouds.enabled=false;
    p[20]=conservative(take(PathMediumSnapshot::create(air)).extinction_majorant());
    p[30]=conservative(medium.extinction_majorant());
    const auto& c=e.clouds;
    p[22]=integers(e.enabled?1u:0u,medium.field()?c.weather_resolution:0u,c.base_resolution,c.detail_resolution);
    p[23]={float(c.bottom_height_m),float(c.top_height_m),float(c.coverage),float(c.density)};
    p[24]={float(c.extinction_per_m),float(medium.settings().cloud_albedo),float(medium.settings().cloud_anisotropy),float(c.top_fade_fraction)};
    p[25][3]=float(c.bottom_fade_fraction);
    const std::array scales{c.weather_scale_m,c.shape_scale_m,c.detail_scale_m};
    for(unsigned i=0;i<3;++i) { const double inverse=1/scales[i]; p[25][i]=float(inverse); p[29][i]=float(inverse-double(p[25][i])); }
    const double width=2*std::pow(std::sin(e.sun.angular_radius_rad/2),2),sine=std::sin(e.sun.angular_radius_rad);
    p[21]={float(width),float(1/(2*std::numbers::pi*width)),float(sine*sine),0};
    if(s.sun) { auto disk=take(solar_disk_radiance(e.sun)); const auto values=pack(rgb(disk)); for(unsigned channel=0;channel<3;++channel) p[4][channel]=values[channel]; }
}
std::array<Float4,3> cloud_origin(const PathMediumSnapshot& m,const Vec3d& origin) {
    std::array<Float4,3> result{}; if(!m.field()) return result;
    const auto& e=m.environment(); const auto& c=e.clouds;
    Vec3d position=origin.cast<float>().cast<double>()-Vec3d{e.atmosphere.planet_center_m.data()};
    position.x()-=c.wind_velocity_mps[0]*m.settings().time_seconds;
    position.z()-=c.wind_velocity_mps[1]*m.settings().time_seconds;
    const auto coordinate=[](double value,double scale) { const double x=value/scale; return float(x-std::floor(x)); };
    result[0]={coordinate(position.x(),c.weather_scale_m),coordinate(position.z(),c.weather_scale_m),0,0};
    for(unsigned i=0;i<3;++i) { result[1][i]=coordinate(position[i],c.shape_scale_m); result[2][i]=coordinate(position[i],c.detail_scale_m); }
    return result;
}
void medium_origin(Parameters& p,const PathMediumSnapshot& m,const Vec3d& origin) {
    p[13]=radial_origin(m,origin); const auto phases=cloud_origin(m,origin);
    for(unsigned i=0;i<3;++i) p[26+i]=phases[i];
}
std::shared_ptr<const PathGpuCloud> prepare_cloud(PathGpuState& state,const PathMediumSnapshot& m) {
    if(!m.field()) return {};
    const auto matches=[&](const auto& cloud) { return cloud && cloud->key==m.field()->key() && cloud->hash==m.field_hash(); };
    if(matches(state.cloud)) return state.cloud;
    for(const auto& view:state.views) if(view && matches(view->cloud)) return view->cloud;
    if(state.active && matches(state.active->owner->cloud)) return state.active->owner->cloud;
    auto candidate=memory::make_shared_in<PathGpuCloud>(state.heap); candidate->key=m.field()->key(); candidate->hash=m.field_hash();
    Vector<std::uint32_t> packed((m.field()->bytes()+3)/4,0,memory::Allocator<std::uint32_t>{state.heap});
    std::size_t at=0;
    for(auto field:{CloudField::weather,CloudField::base,CloudField::detail}) for(auto byte:m.field()->texels(field)) {
        packed[at/4]|=std::uint32_t(byte)<<((at%4)*8); ++at;
    }
    candidate->data=upload(state.factory,std::as_bytes(std::span{packed}),vk::BufferUsageFlagBits::eStorageBuffer);
    return candidate;
}
PathTraceGpuImageInfo medium_info(const PathTraceScene& scene,const PathMediumSnapshot& m,const VolumePathSettings& s,
    std::uint32_t width,std::uint32_t height,PathTraceSamples samples) {
    auto info=info_for(scene,width,height,common_settings(s),samples);
    info.medium=PathTraceGpuMediumInfo{m.environment(),m.settings(),s,m.field_hash()}; return info;
}
}

std::uint64_t PathTraceRenderer::recommended_memory_limit(const Device& device) {
    const auto properties=device.physical_device().getMemoryProperties();
    std::array<VmaBudget,VK_MAX_MEMORY_HEAPS> budgets{};
    vmaGetHeapBudgets(device.allocator(),budgets.data());
    std::uint64_t capacity=0,available=0;
    for(std::uint32_t i=0;i<properties.memoryHeapCount;++i) {
        const auto& heap=properties.memoryHeaps[i];
        if(!(heap.flags&vk::MemoryHeapFlagBits::eDeviceLocal) || heap.size<=capacity) continue;
        capacity=heap.size;
        const auto budget=budgets[i].budget ? std::min(std::uint64_t(budgets[i].budget),capacity) : capacity;
        available=budget>budgets[i].usage ? budget-budgets[i].usage : 0;
    }
    return available;
}
namespace {
void prepare_program(PathGpuState& state,const std::filesystem::path& directory,std::uint32_t capacity,std::stop_token stop) {
    PathGpuState program; program.factory=state.factory;
    const std::array names{"pathMain","pathMiss","shadowMiss","pathHit","pathAnyHit"};
    const std::array stages{ShaderStage::raygeneration,ShaderStage::miss,ShaderStage::miss,ShaderStage::closesthit,ShaderStage::anyhit};
    std::array<ShaderCompileRequest,10> requests;
    const std::array<std::filesystem::path,1> rtxdi_includes{DK_RTXDI_INCLUDE_DIR};
    const auto capacity_text=std::to_string(capacity);
    const std::array defines{ShaderDefine{"DK_PATH_TEXTURES",state.textured ? "1" : "0"},ShaderDefine{"DK_PATH_TEXTURE_CAPACITY",capacity_text}};
    for (std::size_t i=0;i<5;++i) requests[i]={directory/"pathtrace.slang",names[i],stages[i],{},defines};
    requests[5]={directory/"pathtrace-empty.slang","emptyMain",ShaderStage::compute};
    requests[6]={directory/"pathtrace-tone.slang","toneMain",ShaderStage::compute};
    requests[7]={directory/"path-air-empty.slang","airEmptyMain",ShaderStage::compute};
    requests[8]={directory/"path-restir.slang","restirMain",ShaderStage::raygeneration,rtxdi_includes,defines};
    requests[9]={directory/"path-restir-empty.slang","restirEmptyMain",ShaderStage::compute,rtxdi_includes};
    for(auto& request:requests) request.precise_floating_point=true;
    auto compiled=take(compile_shaders(requests,state.heap,stop));
    std::array<ShaderModule,10> shaders; for (std::size_t i=0;i<shaders.size();++i) { cancelled(stop); shaders[i]=take(program.factory.create_shader(compiled[i])); }
    const std::array<const ShaderModule*,5> entries{&shaders[0],&shaders[1],&shaders[2],&shaders[3],&shaders[4]};
    program.layout=take(program.factory.create_pipeline_layout(entries));
    const std::array<const ShaderModule*,2> misses{&shaders[1],&shaders[2]};
    const std::array hits{RayTracingHitGroup{&shaders[3],&shaders[4]},RayTracingHitGroup{&shaders[3],&shaders[4]}};
    program.pipeline=take(program.factory.create_ray_tracing_pipeline({&program.layout,&shaders[0],misses,hits,2}));
    program.table=take(program.factory.create_shader_binding_table(program.pipeline));
    const std::array<const ShaderModule*,1> empty{&shaders[5]}; program.empty_layout=take(program.factory.create_pipeline_layout(empty));
    program.empty_pipeline=take(program.factory.create_compute_pipeline({&shaders[5],&program.empty_layout}));
    const std::array<const ShaderModule*,1> tone{&shaders[6]};
    program.tone_layout=take(program.factory.create_pipeline_layout(tone));
    program.tone_pipeline=take(program.factory.create_compute_pipeline({&shaders[6],&program.tone_layout}));
    const std::array<const ShaderModule*,1> air_empty{&shaders[7]};
    program.air_empty_layout=take(program.factory.create_pipeline_layout(air_empty));
    program.air_empty_pipeline=take(program.factory.create_compute_pipeline({&shaders[7],&program.air_empty_layout}));
    const std::array<const ShaderModule*,5> restir_entries{&shaders[8],&shaders[1],&shaders[2],&shaders[3],&shaders[4]};
    program.restir_layout=take(program.factory.create_pipeline_layout(restir_entries));
    program.restir_pipeline=take(program.factory.create_ray_tracing_pipeline({&program.restir_layout,&shaders[8],misses,hits,2}));
    program.restir_table=take(program.factory.create_shader_binding_table(program.restir_pipeline));
    const std::array<const ShaderModule*,1> restir_empty{&shaders[9]};
    program.restir_empty_layout=take(program.factory.create_pipeline_layout(restir_empty));
    program.restir_empty_pipeline=take(program.factory.create_compute_pipeline({&shaders[9],&program.restir_empty_layout}));
    cancelled(stop);
    state.layout=std::move(program.layout);
    state.empty_layout=std::move(program.empty_layout);
    state.pipeline=std::move(program.pipeline);
    state.table=std::move(program.table);
    state.restir_layout=std::move(program.restir_layout);
    state.restir_empty_layout=std::move(program.restir_empty_layout);
    state.restir_pipeline=std::move(program.restir_pipeline);
    state.restir_table=std::move(program.restir_table);
    state.restir_empty_pipeline=std::move(program.restir_empty_pipeline);
    state.empty_pipeline=std::move(program.empty_pipeline);
    state.tone_pipeline=std::move(program.tone_pipeline);
    state.air_empty_pipeline=std::move(program.air_empty_pipeline);
    state.air_empty_layout=std::move(program.air_empty_layout);
    state.tone_layout=std::move(program.tone_layout);
    state.texture_capacity=capacity;
}
}
Result<PathTraceRenderer> PathTraceRenderer::create(memory::ResourceHandle heap,SubmissionQueue& queue,const std::filesystem::path& directory,std::stop_token stop,std::uint64_t memory_limit) try {
    cancelled(stop);
    if (!heap || heap.state()!=memory::ResourceState::open || !queue.device().ray_tracing_enabled())
        return std::unexpected(Error{ErrorCode::not_supported,"Path renderer requires open Memory and enabled RT"});
    auto state=memory::make_shared_in<detail::PathGpuState>(heap); state->heap=heap; state->factory=queue.resources();
    if(!memory_limit) memory_limit=recommended_memory_limit(queue.device());
    if(memory_limit<(1ull<<20)) return std::unexpected(path_invalid("Path GPU available memory must be at least 1 MiB"));
    state->budget=memory::make_shared_in<AllocationBudget>(heap,memory_limit);
    state->factory=state->factory.with_budget(state->budget);
    state->textured=queue.device().sampled_image_indexing_enabled();
    state->identity=take(state->factory.create_buffer({4}));
    state->shader_directory=directory;
    prepare_program(*state,directory,state->texture_capacity,stop);
    cancelled(stop); return PathTraceRenderer{std::move(state)};
} catch (const Error& error) { return std::unexpected(error); }
catch (const std::exception& error) { return std::unexpected(Error{ErrorCode::internal_error,error.what()}); }

Result<PathTraceGpuScene> PathTraceRenderer::prepare_scene(SubmissionQueue& queue,const PathTraceScene& source,std::stop_token stop) const try {
    ready(state_,queue,stop); const auto* cpu=PathReferenceAccess::get(source);
    if (!cpu) return std::unexpected(path_invalid("Empty path scene"));
    if(!cpu->textures.empty() && !state_->textured) return std::unexpected(Error{ErrorCode::not_supported,"Path textures require enabled sampled-image non-uniform indexing"});
    const auto& limits=queue.device().adapter().properties.limits;
    const auto& as_limits=queue.device().adapter().ray_tracing.acceleration_structure_limits;
    if(cpu->triangles.size()>(1u<<24) || cpu->triangles.size()>as_limits.maxPrimitiveCount || cpu->triangles.size()>limits.maxStorageBufferRange/144)
        throw Error{ErrorCode::not_supported,"Path geometry exceeds device AS/storage buffer capacity"};
    for(const auto& texture:cpu->textures) if(texture.width>limits.maxImageDimension2D || texture.height>limits.maxImageDimension2D)
        throw Error{ErrorCode::not_supported,"Path texture exceeds device image extent"};
    if(cpu->textures.size()>state_->texture_capacity) {
        const auto capacity=cpu->textures.size();
        if(capacity>std::min({limits.maxPerStageDescriptorSamplers,limits.maxDescriptorSetSamplers,
            limits.maxPerStageDescriptorSampledImages,limits.maxDescriptorSetSampledImages}))
            throw Error{ErrorCode::not_supported,"Path texture count exceeds device descriptor capacity"};
        prepare_program(*state_,state_->shader_directory,std::uint32_t(capacity),stop);
    }
    auto scene=memory::make_shared_in<PathGpuScene>(state_->heap); scene->program=state_; scene->source=source;
    for(const auto& weak:state_->geometry_cache) if(auto cached=weak.lock()) {
        if(same_path_geometry(*PathReferenceAccess::get(cached->source),*cpu)) {
            scene->geometry=std::move(cached); cancelled(stop); return PathTraceGpuScene{std::move(scene)};
        }
    }
    scene->geometry=memory::make_shared_in<PathGpuGeometry>(state_->heap,state_->heap); scene->geometry->source=source;
    if (cpu->triangles.empty()) {
        state_->geometry_cache[1]=state_->geometry_cache[0]; state_->geometry_cache[0]=scene->geometry;
        return PathTraceGpuScene{std::move(scene)};
    }
    Vector<Float4> packed(0,memory::Allocator<Float4>{state_->heap});
    Vector<float> vertices(0,memory::Allocator<float>{state_->heap});
    for (const auto& t:cpu->triangles) {
        cancelled(stop); const Vec3f a=t.a.cast<float>(),b=t.b.cast<float>(),c=t.c.cast<float>();
        const Vec3d cross=(b.cast<double>()-a.cast<double>()).cross(c.cast<double>()-a.cast<double>());
        if (!cross.allFinite() || cross.squaredNorm()==0 || cross.dot(t.normal)<=0)
            return std::unexpected(Error{ErrorCode::not_supported,"Path geometry collapses during float conversion"});
        if (float(t.area)<std::numeric_limits<float>::min() || (t.light_weight>0 && float(t.light_weight)<std::numeric_limits<float>::min()))
            return std::unexpected(Error{ErrorCode::not_supported,"Path area/light weight is below normal float range"});
        for (const auto& point:{a,b,c}) for (int k=0;k<3;++k) vertices.push_back(point[k]);
        for (const auto& value:{pack(t.a,t.area),pack(t.b,t.light_weight),pack(t.c,t.light_cdf),pack(t.normal,t.double_sided ? 1 : 0),pack(t.albedo,t.alpha),pack(t.emission,t.cutoff),
            Float4{float(t.uv_a.x()),float(t.uv_a.y()),float(t.uv_b.x()),float(t.uv_b.y())},
            Float4{float(t.uv_c.x()),float(t.uv_c.y()),std::bit_cast<float>(t.texture),t.mask ? 1.f : 0.f},Float4{t.metallic,t.roughness,cpu->material_model==PathTraceMaterialModel::metallic_roughness ? 1.f : 0.f,0}}) packed.push_back(value);
    }
    scene->geometry->triangles=upload(state_->factory,std::as_bytes(std::span{packed}),vk::BufferUsageFlagBits::eStorageBuffer);
    auto staging=upload(state_->factory,std::as_bytes(std::span{vertices}),vk::BufferUsageFlagBits::eTransferSrc);
    auto geometry=take(state_->factory.create_buffer({staging.size(),vk::BufferUsageFlagBits::eTransferDst|
        vk::BufferUsageFlagBits::eShaderDeviceAddress|vk::BufferUsageFlagBits::eAccelerationStructureBuildInputReadOnlyKHR}));
    TriangleGeometry description; description.vertices=&geometry; description.vertex_count=std::uint32_t(vertices.size()/3);
    description.primitive_count=std::uint32_t(cpu->triangles.size()); description.opaque=false;
    scene->geometry->blas=take(state_->factory.create_blas(std::span{&description,1}));
    AccelerationStructureInstance instance; instance.blas=&scene->geometry->blas; instance.flags=vk::GeometryInstanceFlagBitsKHR::eTriangleFacingCullDisable;
    scene->geometry->tlas=take(state_->factory.create_tlas(std::span{&instance,1}));
    auto graph=take(graph::Graph::create(state_->heap));
    const auto input=take(graph.declare_buffer("geometry upload",staging.description(),graph::Lifetime::external,true));
    const auto vertex=take(graph.declare_buffer("world geometry",geometry.description(),graph::Lifetime::external));
    Vector<Buffer> texture_staging(0,memory::Allocator<Buffer>{state_->heap});
    Vector<graph::Use> copy(0,memory::Allocator<graph::Use>{state_->heap});
    copy.push_back(use(input,vk::PipelineStageFlagBits2::eCopy,vk::AccessFlagBits2::eTransferRead));
    copy.push_back(use(vertex,vk::PipelineStageFlagBits2::eCopy,vk::AccessFlagBits2::eTransferWrite,true));
    Vector<graph::ExternalBinding> imports(0,memory::Allocator<graph::ExternalBinding>{state_->heap});
    imports.push_back({0,&staging}); imports.push_back({1,&geometry});
    scene->geometry->texture_count=state_->textured ? std::max<std::size_t>(1,cpu->textures.size()) : 0;
    texture_staging.reserve(scene->geometry->texture_count);
    scene->geometry->textures.resize(scene->geometry->texture_count);
    const auto address=[](TextureWrap w) { return w==TextureWrap::clamp ? vk::SamplerAddressMode::eClampToEdge :
        w==TextureWrap::mirrored_repeat ? vk::SamplerAddressMode::eMirroredRepeat : vk::SamplerAddressMode::eRepeat; };
    const std::array white{Float4{1,1,1,1}};
    for(std::size_t i=0;i<scene->geometry->texture_count;++i) {
        cancelled(stop); const auto* texture=cpu->textures.empty() ? nullptr : &cpu->textures[i];
        Vector<Float4> linear(0,memory::Allocator<Float4>{state_->heap});
        if(texture) {
            linear.resize(std::size_t(texture->width)*texture->height);
            for(std::size_t pixel=0;pixel<linear.size();++pixel) {
                if(pixel%2048==0) cancelled(stop);
                for(std::size_t channel=0;channel<4;++channel) {
                    const double value=std::to_integer<unsigned char>(texture->rgba8[pixel*4+channel])/255.0;
                    linear[pixel][channel]=float(channel==3 ? value : value<=.04045 ? value/12.92 : std::pow((value+.055)/1.055,2.4));
                }
            }
        }
        const auto bytes=texture ? std::as_bytes(std::span<const Float4>{linear}) : std::as_bytes(std::span{white});
        texture_staging.push_back(upload(state_->factory,bytes,vk::BufferUsageFlagBits::eTransferSrc));
        auto& staging_texture=texture_staging.back(); auto& out=scene->geometry->textures[i];
        ImageDesc image_desc{texture ? texture->width : 1,texture ? texture->height : 1,vk::Format::eR32G32B32A32Sfloat};
        image_desc.require_linear_filter=!texture || texture->sampler.mag_filter!=TextureFilter::nearest;
        out.image=take(state_->factory.create_image(image_desc)); out.view=take(state_->factory.create_view(out.image));
        SamplerDesc sampler; sampler.min_filter=sampler.mag_filter=image_desc.require_linear_filter ? vk::Filter::eLinear : vk::Filter::eNearest;
        if(texture) { sampler.address_u=address(texture->sampler.wrap_s); sampler.address_v=address(texture->sampler.wrap_t); }
        out.sampler=take(state_->factory.create_sampler(sampler));
        const auto buffer=take(graph.declare_buffer("texture upload-"+std::to_string(i),staging_texture.description(),graph::Lifetime::external,true));
        const auto image=take(graph.declare_image("baseColor-"+std::to_string(i),image_desc,graph::Lifetime::external));
        copy.push_back(use(buffer,vk::PipelineStageFlagBits2::eCopy,vk::AccessFlagBits2::eTransferRead));
        copy.push_back({image,{{vk::PipelineStageFlagBits2::eCopy,vk::AccessFlagBits2::eTransferWrite,vk::ImageLayout::eTransferDstOptimal},0,VK_WHOLE_SIZE,{vk::ImageAspectFlagBits::eColor,0,1,0,1},true}});
        check(graph.mark_output(image)); imports.push_back({2+i,&staging_texture}); imports.push_back({2+scene->geometry->texture_count+i,nullptr,&out.image});
    }
    const std::array<graph::ResourceId,1> buffers{vertex}; const auto blas=declare(graph,"BLAS",AccelerationStructureKind::bottom_level,buffers);
    const std::array<graph::ResourceId,1> children{blas}; const auto tlas=declare(graph,"TLAS",AccelerationStructureKind::top_level,children);
    const std::array bottom{graph::build_input(vertex),graph::acceleration_build(blas)},top{graph::acceleration_read(blas,false),graph::acceleration_build(tlas)};
    add(graph,"upload",copy); add(graph,"BLAS",bottom); add(graph,"TLAS",top); check(graph.mark_output(tlas));
    auto plan=take(graph.compile(stop)); BuildContext context{staging.size(),*scene};
    const std::array callbacks{graph::PassCallback{0,build_record,&context},graph::PassCallback{1,build_record,&context},graph::PassCallback{2,build_record,&context}};
    imports.push_back({2+2*scene->geometry->texture_count,nullptr,nullptr,{},&scene->geometry->blas});
    imports.push_back({3+2*scene->geometry->texture_count,nullptr,nullptr,{},&scene->geometry->tlas});
    cancelled(stop); auto execution=take(graph::execute(plan,queue,{imports,callbacks,{}})); scene->geometry->built=execution.submission();
    state_->geometry_cache[1]=state_->geometry_cache[0]; state_->geometry_cache[0]=scene->geometry;
    return PathTraceGpuScene{std::move(scene)};
} catch (const Error& error) { return std::unexpected(error); }
catch (const std::exception& error) { return std::unexpected(Error{ErrorCode::internal_error,error.what()}); }

Result<PathTraceFrame> PathTraceRenderer::submit_rays(SubmissionQueue& queue,const PathTraceGpuScene& scene,std::span<const PathTraceRay> rays,
    const PathTraceSettings& settings,PathTraceSamples samples,std::uint32_t first_pixel,std::stop_token stop) const try {
    ready(state_,queue,stop);
    if (!scene || scene.state_->program.lock()!=state_) return std::unexpected(path_invalid("Path scene belongs to a different renderer"));
    const auto& cpu=*PathReferenceAccess::get(scene.source()); check(validate_path_work(cpu,rays.size(),settings,samples,false));
    if (rays.size()-1>std::numeric_limits<std::uint32_t>::max()-first_pixel) return std::unexpected(path_invalid("Path pixel counter overflow"));
    Vector<Float4> packed(0,memory::Allocator<Float4>{state_->heap});
    for (const auto& ray:rays) {
        check(validate_path_ray(ray)); cancelled(stop);
        const double maximum=std::min(ray.maximum,double(std::numeric_limits<float>::max()));
        if (ray.minimum>maximum) return std::unexpected(path_invalid("Path ray interval exceeds float domain"));
        packed.push_back(pack(ray.origin,ray.minimum==0 ? 0.0 : ray.minimum)); packed.push_back(pack(ray.direction,maximum));
    }
    const auto width=std::uint32_t(rays.size()); auto p=parameters(cpu,settings,samples,width,1,true,first_pixel);
    auto info=info_for(scene.source(),width,1,settings,samples); info.fixed_rays=true; info.first_pixel=first_pixel;
    return PathTraceFrame{submit(*state_,queue,*scene.state_,p,packed,std::move(info),stop)};
} catch (const Error& error) { return std::unexpected(error); }
catch (const std::exception& error) { return std::unexpected(Error{ErrorCode::internal_error,error.what()}); }

Result<PathTraceFrame> PathTraceRenderer::submit_view(SubmissionQueue& queue,const PathTraceGpuScene& scene,const RenderView& view,
    const PathTraceSettings& settings,PathTraceSamples samples,std::stop_token stop) const try {
    ready(state_,queue,stop);
    if (!scene || scene.state_->program.lock()!=state_) return std::unexpected(path_invalid("Path scene belongs to a different renderer"));
    const auto& cpu=*PathReferenceAccess::get(scene.source()); check(validate_path_view(cpu,view));
    const auto& d=view.description(); check(validate_path_work(cpu,std::uint64_t{d.width}*d.height,settings,samples,false));
    auto p=view_parameters(cpu,view,settings,samples);
    auto info=info_for(scene.source(),d.width,d.height,settings,samples); info.view=d;
    const std::array<Float4,2> dummy{}; return PathTraceFrame{submit(*state_,queue,*scene.state_,p,dummy,std::move(info),stop)};
} catch (const Error& error) { return std::unexpected(error); }
catch (const std::exception& error) { return std::unexpected(Error{ErrorCode::internal_error,error.what()}); }

namespace {
std::shared_ptr<PathGpuView>* view_slot(PathGpuState& state,std::uint64_t id) {
    for(auto& view:state.views) if(view && view->id==id) return &view;
    return nullptr;
}
bool current(const PathGpuState& state,const PathGpuBatch& batch) {
    return !batch.discarded && !batch.stop.stop_requested() && !batch.tile_stop.stop_requested() && batch.owner->generation==batch.generation &&
        std::ranges::find(state.views,batch.owner)!=state.views.end();
}
bool same_integrator(const PathTraceSettings& a,const PathTraceSettings& b) {
    return a.model_version==b.model_version && a.sampler_version==b.sampler_version && a.seed==b.seed &&
        a.max_bounces==b.max_bounces && a.rr_start==b.rr_start && a.environment_rgb==b.environment_rgb &&
        a.sun==b.sun && a.estimator==b.estimator && a.surface_only==b.surface_only;
}
Parameters view_parameters(const PathSceneState& cpu,const RenderView& view,const PathTraceSettings& settings,PathTraceSamples samples) {
    const auto& d=view.description();
    for(const bool right:{false,true}) for(const bool bottom:{false,true}) {
        const auto corner=take(gpu_path_camera_ray(view,right ? d.width-1 : 0,bottom ? d.height-1 : 0,
            {right ? std::nextafter(1.0,0.0) : 0,bottom ? std::nextafter(1.0,0.0) : 0}));
        if((corner.minimum!=0 && corner.minimum<std::numeric_limits<float>::min()) || corner.maximum>std::numeric_limits<float>::max() || corner.maximum<std::numeric_limits<float>::min())
            throw path_invalid("Path camera clipping interval exceeds normal float domain");
    }
    auto p=parameters(cpu,settings,samples,d.width,d.height,false,0);
    const Mat4d inverse=d.projection.inverse(),world=d.camera_world.matrix(); const Vec3d origin=world.block<3,1>(0,3);
    const bool ortho=d.projection(3,3)==1;
    const auto near_point=[&](double x,double y) -> Vec3d { Vec4d v=inverse*Vec4d{x,y,0,1}; v/=v.w(); return world.block<3,3>(0,0)*v.head<3>(); };
    const Vec3d anchor=ortho ? near_point(0,0) : Vec3d::Zero();
    const Vec3d base=near_point(-1,-1)-anchor; p[6]=pack(origin+anchor,ortho ? 1 : 0); p[7]=pack(base);
    p[8]=pack((near_point(1,-1)-anchor-base)/d.width); p[9]=pack((near_point(-1,1)-anchor-base)/d.height);
    const auto center=take(gpu_path_camera_ray(view,0,0,{.5,.5}));
    if(ortho) p[31]=pack(center.direction,center.maximum);
    else p[5][3]=float(center.maximum/center.minimum);
    return p;
}
void path_memory(const PathGpuState& state,const RenderView& view) {
    const auto stats=state.budget->stats(); const auto& d=view.description();
    if(std::uint64_t{d.width}*d.height*128>stats.limit-stats.current)
        throw path_invalid("GPU path candidate exceeds available memory budget");
}
void validate_gpu_view(const Device& device,const PathSceneState& cpu,const RenderView& view) {
    const auto& limits=device.adapter().properties.limits;
    check(validate_path_view(cpu,view,limits.maxImageDimension2D));
    const auto& d=view.description(); const auto pixels=std::uint64_t{d.width}*d.height;
    if(pixels>limits.maxStorageBufferRange/128 || pixels>std::numeric_limits<std::uint32_t>::max()/8)
        throw Error{ErrorCode::not_supported,"Path image exceeds device storage buffer/address capacity"};
}
void validate_progressive(const Device& device,const PathSceneState& cpu,const RenderView& view,const PathTraceSettings& settings,const PathTraceSchedule& schedule) {
    if(!schedule.target_spp || !schedule.batch_spp || schedule.batch_spp>16 ||
        !schedule.tile_edge || schedule.tile_edge>256 || !schedule.tiles_per_submission || schedule.tiles_per_submission>8 ||
        !std::isfinite(schedule.exposure) || std::abs(schedule.exposure)>32)
        throw path_invalid("Invalid path target, batch, tile or exposure");
    validate_gpu_view(device,cpu,view);
    auto validation=settings; validation.max_triangle_tests=1ull<<29;
    check(validate_path_work(cpu,1,validation,{0,1},false));
    if(settings.max_rays<schedule.batch_spp*(2ull*settings.max_bounces+1))
        throw path_invalid("Progressive target or one-pixel batch exceeds work budget");
    (void)view_parameters(cpu,view,settings,{0,1}); // All float-domain checks precede config publication.
}
}
Result<void> PathTraceRenderer::configure_view(SubmissionQueue& queue,std::uint64_t id,const PathTraceGpuScene& scene,
    const RenderView& view,const PathTraceSettings& settings,const PathTraceSchedule& schedule) try {
    ready(state_,queue,{});
    if(state_->terminal) return std::unexpected(Error{ErrorCode::invalid_state,"Path session device lost"});
    if(!scene || scene.state_->program.lock()!=state_) return std::unexpected(path_invalid("Path scene belongs to a different renderer"));
    validate_progressive(queue.device(),*PathReferenceAccess::get(scene.source()),view,settings,schedule);
    auto* slot=view_slot(*state_,id);
    if(slot) {
        const auto& previous=**slot; const auto& a=previous.view.description(); const auto& b=view.description();
        if(!previous.medium && !previous.restir && previous.scene==scene.state_ && a.width==b.width && a.height==b.height && a.camera_world.matrix()==b.camera_world.matrix() &&
            a.projection==b.projection && same_integrator(previous.settings,settings) && previous.schedule.session==schedule.session &&
            previous.schedule.gesture==schedule.gesture) {
            (*slot)->settings=settings; (*slot)->schedule=schedule; return {};
        }
    } else {
        for(auto& item:state_->views) if(!item) { slot=&item; break; }
        if(!slot) return std::unexpected(Error{ErrorCode::conflict,"Path renderer has two occupied view slots"});
    }
    path_memory(*state_,view);
    auto candidate=memory::make_shared_in<PathGpuView>(state_->heap,view);
    candidate->id=id; candidate->scene=scene.state_; candidate->settings=settings; candidate->schedule=schedule;
    candidate->generation=state_->generation+1;
    *slot=std::move(candidate); ++state_->generation; return {};
} catch(const Error& error) { return std::unexpected(error); }
catch(const std::exception& error) { return std::unexpected(Error{ErrorCode::internal_error,error.what()}); }

Result<PathTraceFrame> PathTraceRenderer::submit_medium_rays(SubmissionQueue& queue,const PathTraceGpuScene& scene,
    const PathMediumSnapshot& medium,std::span<const PathTraceRay> rays,const VolumePathSettings& settings,
    PathTraceSamples samples,std::uint32_t first_pixel,std::stop_token stop) const try {
    ready(state_,queue,stop);
    if(!scene || scene.state_->program.lock()!=state_) throw path_invalid("Path scene belongs to a different renderer");
    validate_medium(scene.source(),medium,settings,samples,rays.size());
    if(rays.size()-1>std::numeric_limits<std::uint32_t>::max()-first_pixel) throw path_invalid("Path pixel counter overflow");
    Vector<Float4> packed(0,memory::Allocator<Float4>{state_->heap});
    for(const auto& ray:rays) {
        check(validate_path_ray(ray)); cancelled(stop);
        const double maximum=std::min(ray.maximum,double(std::numeric_limits<float>::max()));
        if(ray.minimum>maximum) throw path_invalid("Path ray interval exceeds float domain");
        packed.push_back(pack(ray.origin,ray.minimum==0 ? 0.0 : ray.minimum)); packed.push_back(pack(ray.direction,maximum));
        packed.push_back(radial_origin(medium,ray.origin));
        for(const auto& phase:cloud_origin(medium,ray.origin)) packed.push_back(phase);
    }
    const auto width=std::uint32_t(rays.size());
    auto p=parameters(*PathReferenceAccess::get(scene.source()),common_settings(settings),samples,width,1,true,first_pixel);
    medium_parameters(p,medium,settings);
    auto info=medium_info(scene.source(),medium,settings,width,1,samples); info.fixed_rays=true; info.first_pixel=first_pixel;
    return PathTraceFrame{submit(*state_,queue,*scene.state_,p,packed,std::move(info),stop,nullptr,nullptr,prepare_cloud(*state_,medium))};
} catch(const Error& error) { return std::unexpected(error); }
catch(const std::exception& error) { return std::unexpected(Error{ErrorCode::internal_error,error.what()}); }
Result<PathTraceFrame> PathTraceRenderer::submit_medium_view(SubmissionQueue& queue,const PathTraceGpuScene& scene,
    const PathMediumSnapshot& medium,const RenderView& view,const VolumePathSettings& settings,PathTraceSamples samples,std::stop_token stop) const try {
    ready(state_,queue,stop);
    if(!scene || scene.state_->program.lock()!=state_) throw path_invalid("Path scene belongs to a different renderer");
    const auto& d=view.description(); validate_medium(scene.source(),medium,settings,samples,std::uint64_t{d.width}*d.height);
    const auto& cpu=*PathReferenceAccess::get(scene.source()); check(validate_path_view(cpu,view));
    auto p=view_parameters(cpu,view,common_settings(settings),samples); medium_parameters(p,medium,settings);
    medium_origin(p,medium,{p[6][0],p[6][1],p[6][2]});
    auto info=medium_info(scene.source(),medium,settings,d.width,d.height,samples); info.view=d;
    const std::array<Float4,3> dummy{}; return PathTraceFrame{submit(*state_,queue,*scene.state_,p,dummy,std::move(info),stop,nullptr,nullptr,prepare_cloud(*state_,medium))};
} catch(const Error& error) { return std::unexpected(error); }
catch(const std::exception& error) { return std::unexpected(Error{ErrorCode::internal_error,error.what()}); }
Result<void> PathTraceRenderer::configure_medium_view(SubmissionQueue& queue,std::uint64_t id,const PathTraceGpuScene& scene,
    const PathMediumSnapshot& medium,const RenderView& view,const VolumePathSettings& settings,const PathTraceSchedule& schedule) try {
    ready(state_,queue,{});
    if(state_->terminal) throw Error{ErrorCode::invalid_state,"Path session device lost"};
    if(!scene || scene.state_->program.lock()!=state_) throw path_invalid("Path scene belongs to a different renderer");
    if(!schedule.target_spp || !schedule.batch_spp || schedule.batch_spp>16 ||
        !schedule.tile_edge || schedule.tile_edge>256 || !schedule.tiles_per_submission || schedule.tiles_per_submission>8 ||
        !std::isfinite(schedule.exposure) || std::abs(schedule.exposure)>32)
        throw path_invalid("Invalid medium target, batch, tile or exposure");
    validate_medium(scene.source(),medium,settings,{0,1},1);
    const auto& cpu=*PathReferenceAccess::get(scene.source()); validate_gpu_view(queue.device(),cpu,view);
    const auto& d=view.description();
    if(settings.max_rays<schedule.batch_spp*(2ull*settings.max_bounces+1)) throw path_invalid("Progressive medium work budget exceeded");
    const auto common=common_settings(settings);
    auto p=view_parameters(cpu,view,common,{0,1}); medium_parameters(p,medium,settings);
    medium_origin(p,medium,{p[6][0],p[6][1],p[6][2]});
    auto* slot=view_slot(*state_,id);
    if(slot) {
        const auto& previous=**slot; const auto& a=previous.view.description();
        if(previous.medium && !previous.restir && previous.scene==scene.state_ && previous.medium->environment()==medium.environment() &&
            previous.medium->settings()==medium.settings() && previous.medium->field_hash()==medium.field_hash() && a.width==d.width && a.height==d.height &&
            a.camera_world.matrix()==d.camera_world.matrix() && a.projection==d.projection && same_integrator(previous.settings,common) &&
            previous.schedule.session==schedule.session && previous.schedule.gesture==schedule.gesture) {
            (*slot)->settings=common; (*slot)->medium_settings=settings; (*slot)->schedule=schedule; return {};
        }
    } else {
        for(auto& item:state_->views) if(!item) { slot=&item; break; }
        if(!slot) throw Error{ErrorCode::conflict,"Path renderer has two occupied view slots"};
    }
    path_memory(*state_,view);
    auto candidate=memory::make_shared_in<PathGpuView>(state_->heap,view);
    candidate->id=id; candidate->scene=scene.state_; candidate->medium=medium; candidate->medium_settings=settings;
    candidate->cloud=prepare_cloud(*state_,medium);
    candidate->settings=common; candidate->schedule=schedule; candidate->generation=state_->generation+1;
    if(candidate->cloud) state_->cloud=candidate->cloud;
    *slot=std::move(candidate); ++state_->generation; return {};
} catch(const Error& error) { return std::unexpected(error); }
catch(const std::exception& error) { return std::unexpected(Error{ErrorCode::internal_error,error.what()}); }

Result<bool> PathTraceRenderer::submit_batch(SubmissionQueue& queue,std::uint64_t id,std::stop_token stop) try {
    if(stop.stop_requested() && state_ && state_->active && state_->active->owner->id==id) state_->active->discarded=true;
    ready(state_,queue,stop);
    if(state_->terminal) return std::unexpected(Error{ErrorCode::invalid_state,"Path session device lost"});
    auto* slot=view_slot(*state_,id); if(!slot) return std::unexpected(path_invalid("Unknown path view"));
    if(state_->active && !current(*state_,*state_->active) && !state_->active->frame) state_->active.reset();
    if(state_->active && (state_->active->frame || state_->active->owner!=*slot)) return false;
    auto view=*slot; if(view->schedule.paused) return false;
    auto batch=state_->active;
    if(!batch) {
        const auto completed=view->completed ? view->completed->info.samples.count : 0;
        if(completed>=view->schedule.target_spp) return false;
        batch=memory::make_shared_in<PathGpuBatch>(state_->heap); batch->owner=view; batch->generation=view->generation;
        batch->history=view->completed; batch->first=completed;
        batch->sampling_history=view->sampling_history;
        batch->count=std::min(view->schedule.batch_spp,view->schedule.target_spp-completed); batch->stop=stop;
        batch->reverse_tiles=view->schedule.reverse_tiles;
        auto pixels=view->settings.max_rays/((view->restir ? 1 : batch->count)*(2ull*view->settings.max_bounces+(view->restir && !view->medium ? 2 : 1)));
        if(view->restir) pixels=std::min(pixels,view->restir->max_candidate_evaluations/restir_evaluations(*view->restir));
        batch->edge=std::min(view->schedule.tile_edge,std::uint32_t(std::sqrt(double(pixels))));
        const auto tile_pixels=std::uint64_t{batch->edge}*batch->edge;
        const auto ray_work=tile_pixels*(view->restir ? 1 : batch->count)*
            (2ull*view->settings.max_bounces+(view->restir && !view->medium ? 2 : 1));
        auto group_limit=std::min(std::uint64_t{view->schedule.tiles_per_submission},view->settings.max_rays/ray_work);
        if(view->restir) group_limit=std::min(group_limit,view->restir->max_candidate_evaluations/(tile_pixels*restir_evaluations(*view->restir)));
        batch->tiles_per_submission=std::uint32_t(group_limit);
        const auto& d=view->view.description(); batch->columns=(d.width+batch->edge-1)/batch->edge; batch->rows=(d.height+batch->edge-1)/batch->edge;
        if(view->restir) restir_memory(*state_,std::uint64_t{d.width}*d.height,batch->count,*view->restir);
        batch->candidate=memory::make_shared_in<PathGpuCompleted>(state_->heap);
        auto& frame=*batch->candidate; frame.heap=state_->heap; frame.factory=state_->factory; frame.source=view->scene->source;
        frame.info=info_for(frame.source,d.width,d.height,view->settings,{0,completed+batch->count}); frame.info.view=d;
        frame.info.restir=view->restir;
        frame.info.sampling_first_epoch=view->epoch_origin;
        if(batch->history) {
            frame.info.temporal_statistics=batch->history->info.temporal_statistics;
            frame.info.spatial_statistics=batch->history->info.spatial_statistics;
            frame.info.restir_medium_statistics=batch->history->info.restir_medium_statistics;
            frame.info.temporal_history_epoch=batch->history->info.temporal_history_epoch;
        } else if(batch->sampling_history) frame.info.temporal_history_epoch=view->epoch_origin-1;
        if(view->medium) frame.info.medium=PathTraceGpuMediumInfo{view->medium->environment(),view->medium->settings(),view->medium_settings,view->medium->field_hash()};
        frame.buffer=take(state_->factory.create_buffer({std::uint64_t{d.width}*d.height*128,
            vk::BufferUsageFlagBits::eStorageBuffer|vk::BufferUsageFlagBits::eTransferSrc}));
        if(view->restir) {
            const auto n=std::uint64_t{d.width}*d.height;
            frame.surface=take(state_->factory.create_buffer({n*64,vk::BufferUsageFlagBits::eStorageBuffer}));
            frame.reservoir=take(state_->factory.create_buffer({n*32,vk::BufferUsageFlagBits::eStorageBuffer}));
            if(view->restir->spatial) batch->spatial_output=take(state_->factory.create_buffer({n*32,vk::BufferUsageFlagBits::eStorageBuffer}));
            if(batch->count>1) batch->scratch=take(state_->factory.create_buffer({n*128,vk::BufferUsageFlagBits::eStorageBuffer|vk::BufferUsageFlagBits::eTransferSrc}));
            if(batch->count>1 && view->restir->temporal) {
                batch->scratch_surface=take(state_->factory.create_buffer({n*64,vk::BufferUsageFlagBits::eStorageBuffer}));
                batch->scratch_reservoir=take(state_->factory.create_buffer({n*32,vk::BufferUsageFlagBits::eStorageBuffer}));
            }
        }
    }
    const auto& d=view->view.description();
    const auto tile_index=batch->reverse_tiles ? batch->columns*batch->rows-1-batch->next_tile : batch->next_tile;
    const auto x=(tile_index%batch->columns)*batch->edge,y=(tile_index/batch->columns)*batch->edge;
    const auto width=std::min(batch->edge,d.width-x),height=std::min(batch->edge,d.height-y);
    // Freeze the integrator for this batch; later scheduling/budget edits affect the next batch.
    const auto& frozen=batch->candidate->info.settings;
    auto p=view_parameters(*PathReferenceAccess::get(view->scene->source),view->view,frozen,{batch->first,batch->count});
    p[0]=integers(width,height,batch->first,batch->count); p[11]=integers(x,y,d.width,1);
    auto info=info_for(view->scene->source,width,height,frozen,{batch->first,batch->count});
    info.restir=batch->candidate->info.restir;
    const Buffer* old_surface=nullptr; const Buffer* old_reservoir=nullptr;
    if(info.restir) {
        p[0]=integers(width,height,batch->first+batch->epoch,1); p[32]=integers(info.restir->initial_candidates,batch->phase,0,0);
        info.samples={batch->first+batch->epoch,1};
        p[33]=integers(view->epoch_origin,0,0,0);
        p[39]=integers(d.width,d.height,0,0);
        if(info.restir->spatial) {
            const auto& spatial=*info.restir->spatial;
            p[40]=integers(spatial.neighbor_count,spatial.radius,spatial.max_source_candidates,1);
            p[41]={float(spatial.position_tolerance),float(spatial.normal_cosine),0,0};
        }
        if(info.restir->temporal) {
            const auto& t=*info.restir->temporal;
            const bool previous=batch->epoch || bool(batch->sampling_history);
            p[33]=integers(view->epoch_origin,1,previous ? 1u : 0u,t.max_history_age);
            p[34]={float(t.position_tolerance),float(t.normal_cosine),std::bit_cast<float>(t.max_history_candidates),
                std::bit_cast<float>(view->epoch_origin+batch->first+batch->epoch-1)};
            p[39]=integers(d.width,d.height,0,0);
            if(previous) {
                const auto& source=batch->epoch ? d : batch->sampling_history->info.view;
                const Mat4d clip=source.projection*source.camera_world.matrix().inverse();
                for(unsigned row=0;row<4;++row) p[35+row]=pack(clip.block<1,3>(row,0).transpose(),clip(row,3));
                old_surface=batch->epoch ? &batch->scratch_surface : &batch->sampling_history->surface;
                old_reservoir=batch->epoch ? &batch->scratch_reservoir : &batch->sampling_history->reservoir;
            }
        }
    }
    if(view->medium) {
        const auto& air=*batch->candidate->info.medium; medium_parameters(p,*view->medium,air.settings);
        medium_origin(p,*view->medium,{p[6][0],p[6][1],p[6][2]}); info.medium=air;
    }
    const std::array<Float4,3> dummy{};
    const auto group_count=std::min(batch->tiles_per_submission,batch->columns*batch->rows-batch->next_tile);
    std::array<Parameters,8> grouped;
    for(std::uint32_t i=0;i<group_count;++i) {
        cancelled(batch->stop); cancelled(stop);
        const auto index=batch->reverse_tiles ? batch->columns*batch->rows-1-batch->next_tile-i : batch->next_tile+i;
        const auto tx=(index%batch->columns)*batch->edge,ty=(index/batch->columns)*batch->edge;
        grouped[i]=p;
        grouped[i][0][0]=std::bit_cast<float>(std::min(batch->edge,d.width-tx));
        grouped[i][0][1]=std::bit_cast<float>(std::min(batch->edge,d.height-ty));
        grouped[i][11]=integers(tx,ty,d.width,1);
    }
    cancelled(batch->stop);
    auto frame=submit(*state_,queue,*view->scene,p,dummy,std::move(info),stop,&batch->candidate->buffer,
        view->restir && batch->epoch ? &batch->scratch : (batch->history ? &batch->history->buffer : nullptr),view->cloud,
        view->restir ? &batch->candidate->surface : nullptr,
        view->restir ? (batch->phase==4 ? &batch->spatial_output : &batch->candidate->reservoir) : nullptr,old_surface,old_reservoir,
        batch->phase==4 ? &batch->candidate->reservoir : nullptr,std::span{grouped}.first(group_count),batch->stop);
    batch->tile_stop=stop; batch->frame=std::move(frame); state_->active=std::move(batch); return true;
} catch(const Error& error) {
    if(state_ && queue.stats().device_lost) state_->terminal=true;
    if(state_ && state_->active && !state_->active->frame) state_->active.reset();
    return std::unexpected(error);
} catch(const std::exception& error) {
    if(state_ && state_->active && !state_->active->frame) state_->active.reset();
    return std::unexpected(Error{ErrorCode::internal_error,error.what()});
}
Result<bool> PathTraceRenderer::poll(SubmissionQueue& queue,std::stop_token stop) try {
    if(!state_ || state_->heap.state()!=memory::ResourceState::open)
        return std::unexpected(Error{ErrorCode::invalid_state,"Path poll requires open Memory"});
    if(!state_->active) return false;
    auto batch=state_->active;
    if(stop.stop_requested()) batch->discarded=true;
    if(!batch->frame) { if(!current(*state_,*batch)) state_->active.reset(); return false; }
    auto waited=queue.wait(batch->frame->ticket,0);
    if(!waited) {
        if(waited.error().code!=ErrorCode::invalid_argument) {
            batch->discarded=true; state_->terminal=queue.stats().device_lost;
            if(state_->terminal) state_->active.reset();
        }
        return std::unexpected(waited.error());
    }
    if(!*waited) return false;
    if(!current(*state_,*batch)) { state_->active.reset(); return false; }
    const auto frame=batch->frame;
    Vector<std::uint32_t> diagnostic(frame->diagnostic_bytes/sizeof(std::uint32_t),memory::Allocator<std::uint32_t>{state_->heap});
    check(frame->readback.read(0,std::as_writable_bytes(std::span{diagnostic})));
    std::uint32_t error=0;
    for(std::uint32_t i=0;i<frame->tile_count;++i) error|=diagnostic[i*frame->diagnostic_stride/sizeof(std::uint32_t)];
    batch->frame.reset();
    if(error) { state_->active.reset(); return std::unexpected(sample_error(error)); }
    if(!current(*state_,*batch)) { state_->active.reset(); return false; }
    if(batch->candidate->info.restir) for(std::uint32_t i=0;i<frame->tile_count;++i)
        append_restir_statistics(batch->candidate->info,std::span{diagnostic}.subspan(i*frame->diagnostic_stride/sizeof(std::uint32_t),restir_status_words));
    batch->next_tile+=frame->tile_count;
    if(batch->next_tile!=batch->columns*batch->rows) return false;
    if(batch->candidate->info.restir) {
        const auto after_temporal=batch->candidate->info.restir->spatial ? 4u : 1u;
        if(batch->phase==0) { batch->phase=batch->candidate->info.restir->temporal ? 3 : after_temporal; batch->next_tile=0; return false; }
        if(batch->phase==3) { batch->phase=after_temporal; batch->next_tile=0; return false; }
        if(batch->phase==4) {
            std::swap(batch->candidate->reservoir,batch->spatial_output);
            batch->phase=1; batch->next_tile=0; return false;
        }
        if(++batch->epoch<batch->count) {
            if(batch->candidate->info.restir->temporal) {
                std::swap(batch->candidate->surface,batch->scratch_surface);
                std::swap(batch->candidate->reservoir,batch->scratch_reservoir);
            }
            std::swap(batch->candidate->buffer,batch->scratch); batch->phase=0; batch->next_tile=0; return false;
        }
    }
    if(batch->candidate->info.restir && !batch->candidate->info.restir->temporal) {
        // Only temporal reuse consumes these planes after this epoch. Retained completed
        // frames still own their immutable accumulation and provenance, charged in full.
        batch->candidate->surface={}; batch->candidate->reservoir={};
    }
    batch->owner->completed=std::move(batch->candidate);
    if(batch->owner->restir && batch->owner->restir->temporal) batch->owner->sampling_history=batch->owner->completed;
    state_->active.reset(); return true;
} catch(const Error& error) { return std::unexpected(error); }
catch(const std::exception& error) { return std::unexpected(Error{ErrorCode::internal_error,error.what()}); }
Result<void> PathTraceRenderer::reset(std::uint64_t id) {
    if(!state_) return std::unexpected(path_invalid("Empty path renderer"));
    auto* slot=view_slot(*state_,id); if(!slot) return std::unexpected(path_invalid("Unknown path view"));
    (*slot)->generation=++state_->generation; (*slot)->completed.reset(); (*slot)->sampling_history.reset(); (*slot)->epoch_origin=0; return {};
}
Result<void> PathTraceRenderer::cancel(std::uint64_t id) {
    if(!state_) return std::unexpected(path_invalid("Empty path renderer"));
    auto* slot=view_slot(*state_,id); if(!slot) return std::unexpected(path_invalid("Unknown path view"));
    (*slot)->generation=++state_->generation; return {};
}
Result<void> PathTraceRenderer::release_view(std::uint64_t id) {
    if(!state_) return std::unexpected(path_invalid("Empty path renderer"));
    auto* slot=view_slot(*state_,id); if(!slot) return std::unexpected(path_invalid("Unknown path view"));
    slot->reset(); return {};
}
Submission PathTraceRenderer::pending_submission() const noexcept {
    return state_ && state_->active && state_->active->frame ? state_->active->frame->ticket : Submission{};
}
Result<PathTraceProgress> PathTraceRenderer::status(std::uint64_t id) const {
    if(!state_) return std::unexpected(path_invalid("Empty path renderer"));
    auto* slot=view_slot(*state_,id); if(!slot) return std::unexpected(path_invalid("Unknown path view"));
    const auto& v=**slot; PathTraceProgress result; result.view_id=id; result.generation=v.generation;
    result.completed_spp=v.completed ? v.completed->info.samples.count : 0; result.target_spp=v.schedule.target_spp;
    result.paused=v.schedule.paused; result.exposure=v.schedule.exposure; result.device_lost=state_->terminal;
    if(state_->active && state_->active->owner==*slot && current(*state_,*state_->active)) {
        const auto& b=*state_->active; result.pending=true; result.submitted_spp=b.first+b.count;
        result.completed_tiles=b.next_tile; result.total_tiles=b.columns*b.rows;
        if(b.candidate->info.restir) { result.sampling_epoch=v.epoch_origin+b.first+b.epoch; result.sampling_pass=b.phase; }
    }
    return result;
}
PathTraceCompletedFrame PathTraceRenderer::completed_frame(std::uint64_t id) const noexcept {
    if(state_) if(auto* slot=view_slot(*state_,id)) return PathTraceCompletedFrame{(*slot)->completed};
    return {};
}
Result<PathTraceDisplayFrame> PathTraceRenderer::display(SubmissionQueue& queue,const PathTraceCompletedFrame& frame,
    double exposure,bool encode_srgb,std::stop_token stop) const try {
    ready(state_,queue,stop);
    if(!frame || !std::isfinite(exposure) || std::abs(exposure)>32)
        return std::unexpected(path_invalid("Invalid completed frame or display exposure"));
    const auto& info=frame.info();
    const Float4 values{float(info.width),float(info.height),float(std::exp2(exposure)),encode_srgb ? 1.f : 0.f};
    auto uniform=upload(state_->factory,std::as_bytes(std::span{&values,1}),vk::BufferUsageFlagBits::eStorageBuffer);
    ImageDesc description{info.width,info.height,vk::Format::eR8G8B8A8Unorm,
        vk::ImageUsageFlagBits::eStorage|vk::ImageUsageFlagBits::eSampled|vk::ImageUsageFlagBits::eTransferSrc};
    PathTraceDisplayFrame result; result.image=take(state_->factory.create_image(description));
    auto view=take(state_->factory.create_view(result.image));
    const std::array writes{BindingWrite{0,0,BufferBinding{&uniform}},BindingWrite{1,0,BufferBinding{frame.linear_buffer()}},
        BindingWrite{2,0,ImageBinding{&view,vk::ImageLayout::eGeneral}}};
    auto bindings=take(state_->factory.create_bindings(state_->tone_layout,0,writes));
    auto graph=take(graph::Graph::create(state_->heap));
    auto params=take(graph.declare_buffer("tone parameters",uniform.description(),graph::Lifetime::external,true));
    auto history=take(graph.declare_buffer("completed linear",frame.linear_buffer()->description(),graph::Lifetime::external,true));
    auto image=take(graph.declare_image("tone image",description,graph::Lifetime::external));
    const auto stage=vk::PipelineStageFlagBits2::eComputeShader;
    const std::array uses{use(params,stage,vk::AccessFlagBits2::eShaderRead),use(history,stage,vk::AccessFlagBits2::eShaderRead),
        graph::Use{image,{{stage,vk::AccessFlagBits2::eShaderWrite,vk::ImageLayout::eGeneral},0,VK_WHOLE_SIZE,{vk::ImageAspectFlagBits::eColor,0,1,0,1},true}}};
    add(graph,"path tone",uses); check(graph.mark_output(image));
    auto plan=take(graph.compile(stop));
    struct Context { const PathGpuState& state; const BindingSet& bindings; std::uint32_t width,height; } context{*state_,bindings,info.width,info.height};
    const auto record=[](graph::PassContext& pass,void* pointer)->Result<void> {
        const auto& c=*static_cast<Context*>(pointer);
        try { auto encoder=take(pass.compute()); check(encoder.bind_pipeline(c.state.tone_pipeline));
            const std::array<const BindingSet*,1> sets{&c.bindings}; check(encoder.bind_sets(sets));
            return encoder.dispatch({(c.width+15)/16,(c.height+15)/16,1});
        } catch(const Error& error) { return std::unexpected(error); }
    };
    const std::array imports{graph::ExternalBinding{0,&uniform},graph::ExternalBinding{1,frame.linear_buffer()},graph::ExternalBinding{2,nullptr,&result.image}};
    const std::array callbacks{graph::PassCallback{0,record,&context}};
    const std::array final{graph::FinalAccess{2,{{vk::PipelineStageFlagBits2::eFragmentShader,vk::AccessFlagBits2::eShaderSampledRead,vk::ImageLayout::eShaderReadOnlyOptimal}}}};
    cancelled(stop); auto execution=take(graph::execute(plan,queue,{imports,callbacks,final})); result.submission=execution.submission(); return result;
} catch(const Error& error) { return std::unexpected(error); }
catch(const std::exception& error) { return std::unexpected(Error{ErrorCode::internal_error,error.what()}); }
PathTraceMemoryStats PathTraceRenderer::memory_stats(const SubmissionQueue& queue) const noexcept {
    PathTraceMemoryStats stats; if(state_) stats.owned=state_->budget->stats();
    VmaTotalStatistics total{}; vmaCalculateStatistics(queue.device().allocator(),&total);
    stats.device_allocation_bytes=total.total.statistics.allocationBytes; stats.device_block_bytes=total.total.statistics.blockBytes;
    return stats;
}
const PathTraceGpuImageInfo& PathTraceCompletedFrame::info() const noexcept {
    static const PathTraceGpuImageInfo empty; return state_ ? state_->info : empty;
}
const Buffer* PathTraceCompletedFrame::linear_buffer() const noexcept { return state_ ? &state_->buffer : nullptr; }
Result<PathTraceFrame> PathTraceCompletedFrame::read_linear(SubmissionQueue& queue,std::stop_token stop) const try {
    cancelled(stop);
    if(!state_) return std::unexpected(path_invalid("Empty completed path frame"));
    auto frame=memory::make_shared_in<PathGpuFrame>(state_->heap); frame->heap=state_->heap;
    frame->source=state_->source; frame->info=state_->info; frame->stride=128;
    frame->readback=take(state_->factory.create_buffer({state_->buffer.size(),vk::BufferUsageFlagBits::eTransferDst,BufferMemory::readback}));
    auto batch=take(queue.begin()); check(batch.copy(state_->buffer,frame->readback,state_->buffer.size()));
    cancelled(stop); frame->ticket=take(queue.submit(std::move(batch))); return PathTraceFrame{std::move(frame)};
} catch(const Error& error) { return std::unexpected(error); }
catch(const std::exception& error) { return std::unexpected(Error{ErrorCode::internal_error,error.what()}); }

Submission PathTraceGpuScene::submission() const noexcept { return state_ ? state_->geometry->built : Submission{}; }
const PathTraceScene& PathTraceGpuScene::source() const noexcept { static const PathTraceScene empty; return state_ ? state_->source : empty; }
Submission PathTraceFrame::submission() const noexcept { return state_ ? state_->ticket : Submission{}; }
Result<std::optional<PathTraceGpuImage>> PathTraceFrame::read(SubmissionQueue& queue,std::stop_token stop) const try {
    cancelled(stop);
    if (!state_ || state_->heap.state()!=memory::ResourceState::open) return std::unexpected(Error{ErrorCode::invalid_state,"Frame read requires open Memory"});
    if (!take(queue.wait(state_->ticket,0))) return std::optional<PathTraceGpuImage>{};
    if(state_->diagnostic_offset) {
        std::uint32_t diagnostic=0;
        check(state_->readback.read(state_->diagnostic_offset,std::as_writable_bytes(std::span{&diagnostic,1})));
        if(diagnostic) return std::unexpected(sample_error(diagnostic));
    }
    auto image=memory::make_shared_in<PathGpuImage>(state_->heap,state_->heap); image->info=state_->info;
    const auto count=std::size_t{image->info.width}*image->info.height;
    Vector<PathGpuPixel> pixels(count*(state_->stride/sizeof(PathGpuPixel)),memory::Allocator<PathGpuPixel>{state_->heap});
    check(state_->readback.read(0,std::as_writable_bytes(std::span{pixels})));
    image->colors.reserve(count); image->moments.reserve(count); image->hits.reserve(count);
    const auto& scene=*PathReferenceAccess::get(state_->source);
    for (std::size_t i=0;i<count;++i) {
        const auto& pixel=pixels[i*(state_->stride/sizeof(PathGpuPixel))];
        cancelled(stop);
        for (const auto& block:{pixel.mean,pixel.second,pixel.hit,pixel.normal}) for (const auto value:block)
            if (!std::isfinite(value)) return std::unexpected(Error{ErrorCode::internal_error,"Nonfinite GPU path result"});
        const Vec3d color{pixel.mean[0],pixel.mean[1],pixel.mean[2]},moment{pixel.second[0],pixel.second[1],pixel.second[2]};
        double rays=pixel.mean[3];
        if(state_->stride==128) {
            const auto& history=pixels[2*i+1];
            rays=double(std::uint64_t(std::bit_cast<std::uint32_t>(history.second[3])) |
                (std::uint64_t(std::bit_cast<std::uint32_t>(history.hit[3]))<<32));
        }
        const double bound=double(image->info.samples.count)*(2*image->info.settings.max_bounces+(image->info.restir && !image->info.medium ? 2 : 1));
        if ((color.array()<0).any() || (moment.array()<0).any() || pixel.second[3]!=0 || rays<0 || rays>bound || rays!=std::floor(rays))
            return std::unexpected(Error{ErrorCode::internal_error,"Invalid GPU path HDR/statistics"});
        image->info.statistics.rays+=std::uint64_t(rays); image->colors.push_back(color); image->moments.push_back(moment);
        std::optional<PathTraceHit> hit;
        if (pixel.hit[3]!=0) {
            const double index=pixel.hit[3]-1;
            if (index<0 || index>=scene.triangles.size() || index!=std::floor(index) || pixel.hit[0]<0 ||
                pixel.hit[1]<0 || pixel.hit[2]<0 || pixel.hit[1]+pixel.hit[2]>1.00001f)
                return std::unexpected(Error{ErrorCode::internal_error,"Invalid GPU primary hit"});
            const auto& t=scene.triangles[std::size_t(index)]; PathTraceHit h;
            h.entity=t.entity; h.mesh=t.mesh; h.primitive=t.primitive; h.triangle=t.triangle; h.index=std::size_t(index);
            h.distance=pixel.hit[0]; h.barycentric={1-double(pixel.hit[1])-pixel.hit[2],pixel.hit[1],pixel.hit[2]};
            h.position=t.a*h.barycentric.x()+t.b*h.barycentric.y()+t.c*h.barycentric.z();
            h.normal={pixel.normal[0],pixel.normal[1],pixel.normal[2]}; h.front_face=pixel.normal[3]>0;
            if (std::abs(h.normal.squaredNorm()-1)>1e-4 || std::abs(pixel.normal[3])!=1)
                return std::unexpected(Error{ErrorCode::internal_error,"Invalid GPU primary normal"});
            hit=std::move(h);
        }
        image->hits.push_back(std::move(hit));
    }
    cancelled(stop); return std::optional{PathTraceGpuImage{std::move(image)}};
} catch (const Error& error) { return std::unexpected(error); }
catch (const std::exception& error) { return std::unexpected(Error{ErrorCode::internal_error,error.what()}); }
std::span<const Vec3d> PathTraceGpuImage::linear_rgb() const noexcept { return state_ ? std::span<const Vec3d>{state_->colors} : std::span<const Vec3d>{}; }
std::span<const Vec3d> PathTraceGpuImage::second_moment() const noexcept { return state_ ? std::span<const Vec3d>{state_->moments} : std::span<const Vec3d>{}; }
std::span<const std::optional<PathTraceHit>> PathTraceGpuImage::primary_hits() const noexcept { return state_ ? std::span<const std::optional<PathTraceHit>>{state_->hits} : std::span<const std::optional<PathTraceHit>>{}; }
const PathTraceGpuImageInfo& PathTraceGpuImage::info() const noexcept { static const PathTraceGpuImageInfo empty; return state_ ? state_->info : empty; }

Result<PathTraceFrame> PathTraceRenderer::submit_air_rays(graphics::SubmissionQueue& q,const PathTraceGpuScene& scene,
    const PathMediumSnapshot& m,std::span<const PathTraceRay> rays,const VolumePathSettings& s,PathTraceSamples samples,std::uint32_t pixel,std::stop_token stop) const {
    if(m.environment().clouds.enabled) return std::unexpected(Error{ErrorCode::not_supported,"Air paths require clouds disabled; use medium paths"});
    return submit_medium_rays(q,scene,m,rays,s,samples,pixel,stop);
}
Result<PathTraceFrame> PathTraceRenderer::submit_air_view(graphics::SubmissionQueue& q,const PathTraceGpuScene& scene,
    const PathMediumSnapshot& m,const RenderView& view,const VolumePathSettings& s,PathTraceSamples samples,std::stop_token stop) const {
    if(m.environment().clouds.enabled) return std::unexpected(Error{ErrorCode::not_supported,"Air paths require clouds disabled; use medium paths"});
    return submit_medium_view(q,scene,m,view,s,samples,stop);
}
Result<void> PathTraceRenderer::configure_air_view(graphics::SubmissionQueue& q,std::uint64_t id,const PathTraceGpuScene& scene,
    const PathMediumSnapshot& m,const RenderView& view,const VolumePathSettings& s,const PathTraceSchedule& schedule) {
    if(m.environment().clouds.enabled) return std::unexpected(Error{ErrorCode::not_supported,"Air paths require clouds disabled; use medium paths"});
    return configure_medium_view(q,id,scene,m,view,s,schedule);
}
#include "PathRestirGpu.inl"
#include "PathRestirMediumGpu.inl"
} // namespace dk::render

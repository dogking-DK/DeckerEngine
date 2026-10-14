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
struct PathGpuState {
    memory::ResourceHandle heap;
    ResourceFactory factory;
    Buffer identity;
    PipelineLayout layout,empty_layout;
    RayTracingPipeline pipeline;
    ShaderBindingTable table;
    ComputePipeline empty_pipeline;
    bool textured = false;
};
struct PathGpuTexture { Image image; ImageView view; Sampler sampler; };
struct PathGpuScene {
    std::shared_ptr<PathGpuState> program;
    PathTraceScene source;
    Buffer triangles;
    AccelerationStructure blas,tlas;
    Submission built;
    std::array<PathGpuTexture,64> textures;
    std::size_t texture_count = 0;
};
struct PathGpuFrame {
    memory::ResourceHandle heap;
    PathTraceScene source;
    PathTraceGpuImageInfo info;
    Buffer readback;
    Submission ticket;
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
template<class T> T take(Result<T> result) { if (!result) throw result.error(); return std::move(*result); }
void check(Result<void> result) { if (!result) throw result.error(); }
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
    const auto& c=*static_cast<BuildContext*>(context); const auto count=c.scene.texture_count;
    if (pass.pass_index()==0) {
        if(auto copied=pass.copy_buffer(0,1,c.bytes);!copied) return copied;
        for(std::size_t i=0;i<count;++i) {
            const auto d=c.scene.textures[i].image.description();
            if(auto copied=pass.copy_to_image(2+i,2+count+i,{0,0,0,0,d.width,d.height});!copied) return copied;
        }
        return {};
    }
    return pass.build(2*count+pass.pass_index()+1);
}
struct TraceContext {
    const PathGpuState& state; const BindingSet& bindings;
    std::uint32_t width,height; bool empty;
    std::size_t shader_table;
};
Result<void> trace_record(graph::PassContext& pass,void* context) {
    const auto& c=*static_cast<TraceContext*>(context);
    try {
        if (pass.pass_index()==1) return pass.copy_buffer(3,4,std::uint64_t{c.width}*c.height*sizeof(PathGpuPixel));
        const std::array<const BindingSet*,1> sets{&c.bindings};
        if (!c.empty) return pass.trace(c.state.pipeline,c.shader_table,sets,{}, {c.width,c.height,1});
        auto encoder=take(pass.compute()); check(encoder.bind_pipeline(c.state.empty_pipeline)); check(encoder.bind_sets(sets));
        return encoder.dispatch({(c.width*c.height+63)/64,1,1});
    } catch (const Error& error) { return std::unexpected(error); }
}
using Parameters=std::array<Float4,11>;
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
    Parameters p,std::span<const Float4> rays,PathTraceGpuImageInfo info,std::stop_token stop) {
    const bool empty=scene.source.triangle_count()==0;
    const auto count=std::uint64_t{info.width}*info.height,bytes=count*sizeof(PathGpuPixel);
    auto frame=memory::make_shared_in<PathGpuFrame>(state.heap); frame->heap=state.heap; frame->source=scene.source; frame->info=std::move(info);
    const auto storage=vk::BufferUsageFlagBits::eStorageBuffer;
    auto uniform=upload(state.factory,std::as_bytes(std::span{p}),storage);
    auto input=upload(state.factory,std::as_bytes(rays),storage);
    auto output=take(state.factory.create_buffer({bytes,storage|vk::BufferUsageFlagBits::eTransferSrc}));
    frame->readback=take(state.factory.create_buffer({bytes,vk::BufferUsageFlagBits::eTransferDst,BufferMemory::readback}));
    Vector<BindingWrite> writes(0,memory::Allocator<BindingWrite>{state.heap});
    if (!empty) { writes.push_back({0,0,AccelerationStructureBinding{&scene.tlas}}); writes.push_back({1,0,BufferBinding{&scene.triangles}}); writes.push_back({4,0,BufferBinding{&input}}); }
    writes.push_back({2,0,BufferBinding{&uniform}}); writes.push_back({3,0,BufferBinding{&output}});
    if(!empty && state.textured) for(std::uint32_t i=0;i<64;++i) {
        const auto& texture=scene.textures[std::min<std::size_t>(i,scene.texture_count-1)];
        writes.push_back({5,i,ImageBinding{&texture.view}}); writes.push_back({6,i,SamplerBinding{&texture.sampler}});
    }
    auto bindings=take(state.factory.create_bindings(empty ? state.empty_layout : state.layout,0,writes));
    auto g=take(graph::Graph::create(state.heap));
    const auto params=take(g.declare_buffer("parameters",uniform.description(),graph::Lifetime::external,true));
    const auto geometry=take(g.declare_buffer("triangles",empty ? BufferDesc{4,storage} : scene.triangles.description(),graph::Lifetime::external,true));
    const auto ray_input=take(g.declare_buffer("rays",input.description(),graph::Lifetime::external,true));
    const auto result=take(g.declare_buffer("HDR and diagnostics",output.description(),graph::Lifetime::external));
    const auto readback=take(g.declare_buffer("readback",frame->readback.description(),graph::Lifetime::external));
    const auto stage=empty ? vk::PipelineStageFlagBits2::eComputeShader : vk::PipelineStageFlagBits2::eRayTracingShaderKHR;
    Vector<graph::Use> uses(0,memory::Allocator<graph::Use>{state.heap});
    uses.push_back(use(params,stage,vk::AccessFlagBits2::eShaderRead)); uses.push_back(use(result,stage,vk::AccessFlagBits2::eShaderWrite,true));
    Vector<graph::ExternalBinding> imports(0,memory::Allocator<graph::ExternalBinding>{state.heap});
    imports.push_back({0,&uniform}); imports.push_back({3,&output}); imports.push_back({4,&frame->readback});
    for(std::size_t i=0;i<scene.texture_count;++i) {
        const auto& texture=scene.textures[i];
        auto id=take(g.declare_image("baseColor-"+std::to_string(i),texture.image.description(),graph::Lifetime::external,true));
        uses.push_back({id,{{stage,vk::AccessFlagBits2::eShaderSampledRead,vk::ImageLayout::eShaderReadOnlyOptimal}}});
        imports.push_back({5+i,nullptr,&texture.image});
    }
    if (!empty) {
        const auto blas=declare(g,"BLAS",AccelerationStructureKind::bottom_level,{},true);
        const std::array<graph::ResourceId,1> children{blas}; const auto tlas=declare(g,"TLAS",AccelerationStructureKind::top_level,children,true);
        const auto sbt=take(g.declare_shader_table("SBT"));
        uses.push_back(use(geometry,stage,vk::AccessFlagBits2::eShaderRead)); uses.push_back(use(ray_input,stage,vk::AccessFlagBits2::eShaderRead));
        uses.push_back(graph::acceleration_read(blas)); uses.push_back(graph::acceleration_read(tlas)); uses.push_back(graph::shader_table_read(sbt));
        imports.push_back({1,&scene.triangles}); imports.push_back({2,&input});
        const auto base=5+scene.texture_count;
        imports.push_back({base,nullptr,nullptr,{},&scene.blas}); imports.push_back({base+1,nullptr,nullptr,{},&scene.tlas}); imports.push_back({base+2,nullptr,nullptr,{},nullptr,&state.table});
    }
    add(g,empty ? "empty environment" : "surface path trace",uses);
    const std::array copy{use(result,vk::PipelineStageFlagBits2::eCopy,vk::AccessFlagBits2::eTransferRead),use(readback,vk::PipelineStageFlagBits2::eCopy,vk::AccessFlagBits2::eTransferWrite,true)};
    add(g,"HDR readback",copy); check(g.mark_output(readback));
    auto plan=take(g.compile(stop)); TraceContext context{state,bindings,frame->info.width,frame->info.height,empty,7+scene.texture_count};
    const std::array callbacks{graph::PassCallback{0,trace_record,&context},graph::PassCallback{1,trace_record,&context}};
    const std::array final{graph::FinalAccess{4,{{vk::PipelineStageFlagBits2::eHost,vk::AccessFlagBits2::eHostRead}}}};
    cancelled(stop); auto execution=take(graph::execute(plan,queue,{imports,callbacks,final}));
    frame->ticket=execution.submission(); return frame; // No allocation/fallible work after successful submit.
}
PathTraceGpuImageInfo info_for(const PathTraceScene& scene,std::uint32_t width,std::uint32_t height,
    const PathTraceSettings& settings,PathTraceSamples samples) {
    PathTraceGpuImageInfo info; info.width=width; info.height=height; info.scene=scene.id(); info.revision=scene.revision();
    info.fingerprint=scene.fingerprint(); info.texture_count=scene.texture_count(); info.masked_emitter_count=scene.masked_emitter_count();
    info.material_model=scene.material_model(); info.settings=settings; info.samples=samples; return info;
}
}

Result<PathTraceRenderer> PathTraceRenderer::create(memory::ResourceHandle heap,SubmissionQueue& queue,const std::filesystem::path& directory,std::stop_token stop) try {
    cancelled(stop);
    if (!heap || heap.state()!=memory::ResourceState::open || !queue.device().ray_tracing_enabled())
        return std::unexpected(Error{ErrorCode::not_supported,"Path renderer requires open Memory and enabled RT"});
    auto state=memory::make_shared_in<detail::PathGpuState>(heap); state->heap=heap; state->factory=queue.resources();
    state->textured=queue.device().sampled_image_indexing_enabled();
    state->identity=take(state->factory.create_buffer({4}));
    const std::array names{"pathMain","pathMiss","shadowMiss","pathHit","pathAnyHit"};
    const std::array stages{ShaderStage::raygeneration,ShaderStage::miss,ShaderStage::miss,ShaderStage::closesthit,ShaderStage::anyhit};
    std::array<ShaderCompileRequest,6> requests;
    const std::array defines{ShaderDefine{"DK_PATH_TEXTURES",state->textured ? "1" : "0"}};
    for (std::size_t i=0;i<5;++i) requests[i]={directory/"pathtrace.slang",names[i],stages[i],{},defines};
    requests[5]={directory/"pathtrace-empty.slang","emptyMain",ShaderStage::compute};
    for(auto& request:requests) request.precise_floating_point=true;
    auto compiled=take(compile_shaders(requests,heap,stop));
    std::array<ShaderModule,6> shaders; for (std::size_t i=0;i<shaders.size();++i) { cancelled(stop); shaders[i]=take(state->factory.create_shader(compiled[i])); }
    const std::array<const ShaderModule*,5> entries{&shaders[0],&shaders[1],&shaders[2],&shaders[3],&shaders[4]};
    state->layout=take(state->factory.create_pipeline_layout(entries));
    const std::array<const ShaderModule*,2> misses{&shaders[1],&shaders[2]};
    const std::array hits{RayTracingHitGroup{&shaders[3],&shaders[4]},RayTracingHitGroup{&shaders[3],&shaders[4]}};
    state->pipeline=take(state->factory.create_ray_tracing_pipeline({&state->layout,&shaders[0],misses,hits,2}));
    state->table=take(state->factory.create_shader_binding_table(state->pipeline));
    const std::array<const ShaderModule*,1> empty{&shaders[5]}; state->empty_layout=take(state->factory.create_pipeline_layout(empty));
    state->empty_pipeline=take(state->factory.create_compute_pipeline({&shaders[5],&state->empty_layout}));
    cancelled(stop); return PathTraceRenderer{std::move(state)};
} catch (const Error& error) { return std::unexpected(error); }
catch (const std::exception& error) { return std::unexpected(Error{ErrorCode::internal_error,error.what()}); }

Result<PathTraceGpuScene> PathTraceRenderer::prepare_scene(SubmissionQueue& queue,const PathTraceScene& source,std::stop_token stop) const try {
    ready(state_,queue,stop); const auto* cpu=PathReferenceAccess::get(source);
    if (!cpu) return std::unexpected(path_invalid("Empty path scene"));
    if(!cpu->textures.empty() && !state_->textured) return std::unexpected(Error{ErrorCode::not_supported,"Path textures require enabled sampled-image non-uniform indexing"});
    auto scene=memory::make_shared_in<PathGpuScene>(state_->heap); scene->program=state_; scene->source=source;
    if (cpu->triangles.empty()) return PathTraceGpuScene{std::move(scene)};
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
    scene->triangles=upload(state_->factory,std::as_bytes(std::span{packed}),vk::BufferUsageFlagBits::eStorageBuffer);
    auto staging=upload(state_->factory,std::as_bytes(std::span{vertices}),vk::BufferUsageFlagBits::eTransferSrc);
    auto geometry=take(state_->factory.create_buffer({staging.size(),vk::BufferUsageFlagBits::eTransferDst|
        vk::BufferUsageFlagBits::eShaderDeviceAddress|vk::BufferUsageFlagBits::eAccelerationStructureBuildInputReadOnlyKHR}));
    TriangleGeometry description; description.vertices=&geometry; description.vertex_count=std::uint32_t(vertices.size()/3);
    description.primitive_count=std::uint32_t(cpu->triangles.size()); description.opaque=false;
    scene->blas=take(state_->factory.create_blas(std::span{&description,1}));
    AccelerationStructureInstance instance; instance.blas=&scene->blas; instance.flags=vk::GeometryInstanceFlagBitsKHR::eTriangleFacingCullDisable;
    scene->tlas=take(state_->factory.create_tlas(std::span{&instance,1}));
    auto graph=take(graph::Graph::create(state_->heap));
    const auto input=take(graph.declare_buffer("geometry upload",staging.description(),graph::Lifetime::external,true));
    const auto vertex=take(graph.declare_buffer("world geometry",geometry.description(),graph::Lifetime::external));
    Vector<Buffer> texture_staging(0,memory::Allocator<Buffer>{state_->heap});
    Vector<graph::Use> copy(0,memory::Allocator<graph::Use>{state_->heap});
    copy.push_back(use(input,vk::PipelineStageFlagBits2::eCopy,vk::AccessFlagBits2::eTransferRead));
    copy.push_back(use(vertex,vk::PipelineStageFlagBits2::eCopy,vk::AccessFlagBits2::eTransferWrite,true));
    Vector<graph::ExternalBinding> imports(0,memory::Allocator<graph::ExternalBinding>{state_->heap});
    imports.push_back({0,&staging}); imports.push_back({1,&geometry});
    scene->texture_count=state_->textured ? std::max<std::size_t>(1,cpu->textures.size()) : 0;
    texture_staging.reserve(scene->texture_count);
    const auto address=[](TextureWrap w) { return w==TextureWrap::clamp ? vk::SamplerAddressMode::eClampToEdge :
        w==TextureWrap::mirrored_repeat ? vk::SamplerAddressMode::eMirroredRepeat : vk::SamplerAddressMode::eRepeat; };
    const std::array white{Float4{1,1,1,1}};
    for(std::size_t i=0;i<scene->texture_count;++i) {
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
        auto& staging_texture=texture_staging.back(); auto& out=scene->textures[i];
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
        check(graph.mark_output(image)); imports.push_back({2+i,&staging_texture}); imports.push_back({2+scene->texture_count+i,nullptr,&out.image});
    }
    const std::array<graph::ResourceId,1> buffers{vertex}; const auto blas=declare(graph,"BLAS",AccelerationStructureKind::bottom_level,buffers);
    const std::array<graph::ResourceId,1> children{blas}; const auto tlas=declare(graph,"TLAS",AccelerationStructureKind::top_level,children);
    const std::array bottom{graph::build_input(vertex),graph::acceleration_build(blas)},top{graph::acceleration_read(blas,false),graph::acceleration_build(tlas)};
    add(graph,"upload",copy); add(graph,"BLAS",bottom); add(graph,"TLAS",top); check(graph.mark_output(tlas));
    auto plan=take(graph.compile(stop)); BuildContext context{staging.size(),*scene};
    const std::array callbacks{graph::PassCallback{0,build_record,&context},graph::PassCallback{1,build_record,&context},graph::PassCallback{2,build_record,&context}};
    imports.push_back({2+2*scene->texture_count,nullptr,nullptr,{},&scene->blas});
    imports.push_back({3+2*scene->texture_count,nullptr,nullptr,{},&scene->tlas});
    cancelled(stop); auto execution=take(graph::execute(plan,queue,{imports,callbacks,{}})); scene->built=execution.submission();
    return PathTraceGpuScene{std::move(scene)};
} catch (const Error& error) { return std::unexpected(error); }
catch (const std::exception& error) { return std::unexpected(Error{ErrorCode::internal_error,error.what()}); }

Result<PathTraceFrame> PathTraceRenderer::submit_rays(SubmissionQueue& queue,const PathTraceGpuScene& scene,std::span<const PathTraceRay> rays,
    const PathTraceSettings& settings,PathTraceSamples samples,std::uint32_t first_pixel,std::stop_token stop) const try {
    ready(state_,queue,stop);
    if (!scene || scene.state_->program!=state_) return std::unexpected(path_invalid("Path scene belongs to a different renderer"));
    const auto& cpu=*PathReferenceAccess::get(scene.source()); check(validate_path_work(cpu,rays.size(),settings,samples));
    if (rays.size()-1>std::numeric_limits<std::uint32_t>::max()-first_pixel) return std::unexpected(path_invalid("Path pixel counter overflow"));
    Vector<Float4> packed(0,memory::Allocator<Float4>{state_->heap});
    for (const auto& ray:rays) {
        check(validate_path_ray(ray)); cancelled(stop);
        const double maximum=std::min(ray.maximum,double(std::numeric_limits<float>::max()));
        if (ray.minimum>maximum) return std::unexpected(path_invalid("Path ray interval exceeds float domain"));
        packed.push_back(pack(ray.origin,ray.minimum)); packed.push_back(pack(ray.direction,maximum));
    }
    const auto width=std::uint32_t(rays.size()); auto p=parameters(cpu,settings,samples,width,1,true,first_pixel);
    auto info=info_for(scene.source(),width,1,settings,samples); info.fixed_rays=true; info.first_pixel=first_pixel;
    return PathTraceFrame{submit(*state_,queue,*scene.state_,p,packed,std::move(info),stop)};
} catch (const Error& error) { return std::unexpected(error); }
catch (const std::exception& error) { return std::unexpected(Error{ErrorCode::internal_error,error.what()}); }

Result<PathTraceFrame> PathTraceRenderer::submit_view(SubmissionQueue& queue,const PathTraceGpuScene& scene,const RenderView& view,
    const PathTraceSettings& settings,PathTraceSamples samples,std::stop_token stop) const try {
    ready(state_,queue,stop);
    if (!scene || scene.state_->program!=state_) return std::unexpected(path_invalid("Path scene belongs to a different renderer"));
    const auto& cpu=*PathReferenceAccess::get(scene.source()); check(validate_path_view(cpu,view));
    const auto& d=view.description(); check(validate_path_work(cpu,std::uint64_t{d.width}*d.height,settings,samples));
    for (const bool right:{false,true}) for (const bool bottom:{false,true}) {
        const auto corner=take(path_trace_camera_ray(view,right ? d.width-1 : 0,bottom ? d.height-1 : 0,
            {right ? std::nextafter(1.0,0.0) : 0,bottom ? std::nextafter(1.0,0.0) : 0}));
        if (corner.minimum<std::numeric_limits<float>::min() || corner.maximum>std::numeric_limits<float>::max())
            return std::unexpected(path_invalid("Path camera clipping interval exceeds normal float domain"));
    }
    auto p=parameters(cpu,settings,samples,d.width,d.height,false,0);
    const Mat4d inverse=d.projection.inverse(),world=d.camera_world.matrix(); const Vec3d origin=world.block<3,1>(0,3);
    const auto near_point=[&](double x,double y) -> Vec3d { Vec4d v=inverse*Vec4d{x,y,0,1}; v/=v.w(); return (world*v).head<3>()-origin; };
    const Vec3d base=near_point(-1,-1); p[6]=pack(origin); p[7]=pack(base);
    p[8]=pack((near_point(1,-1)-base)/d.width); p[9]=pack((near_point(-1,1)-base)/d.height);
    const auto center=take(path_trace_camera_ray(view,0,0,{.5,.5})); p[5][3]=float(center.maximum/center.minimum);
    auto info=info_for(scene.source(),d.width,d.height,settings,samples); info.view=d;
    const std::array<Float4,2> dummy{}; return PathTraceFrame{submit(*state_,queue,*scene.state_,p,dummy,std::move(info),stop)};
} catch (const Error& error) { return std::unexpected(error); }
catch (const std::exception& error) { return std::unexpected(Error{ErrorCode::internal_error,error.what()}); }

Submission PathTraceGpuScene::submission() const noexcept { return state_ ? state_->built : Submission{}; }
const PathTraceScene& PathTraceGpuScene::source() const noexcept { static const PathTraceScene empty; return state_ ? state_->source : empty; }
Submission PathTraceFrame::submission() const noexcept { return state_ ? state_->ticket : Submission{}; }
Result<std::optional<PathTraceGpuImage>> PathTraceFrame::read(SubmissionQueue& queue,std::stop_token stop) const try {
    cancelled(stop);
    if (!state_ || state_->heap.state()!=memory::ResourceState::open) return std::unexpected(Error{ErrorCode::invalid_state,"Frame read requires open Memory"});
    if (!take(queue.wait(state_->ticket,0))) return std::optional<PathTraceGpuImage>{};
    auto image=memory::make_shared_in<PathGpuImage>(state_->heap,state_->heap); image->info=state_->info;
    const auto count=std::size_t{image->info.width}*image->info.height;
    Vector<PathGpuPixel> pixels(count,memory::Allocator<PathGpuPixel>{state_->heap});
    check(state_->readback.read(0,std::as_writable_bytes(std::span{pixels})));
    image->colors.reserve(count); image->moments.reserve(count); image->hits.reserve(count);
    const auto& scene=*PathReferenceAccess::get(state_->source);
    for (const auto& pixel:pixels) {
        cancelled(stop);
        for (const auto& block:{pixel.mean,pixel.second,pixel.hit,pixel.normal}) for (const auto value:block)
            if (!std::isfinite(value)) return std::unexpected(Error{ErrorCode::internal_error,"Nonfinite GPU path result"});
        const Vec3d color{pixel.mean[0],pixel.mean[1],pixel.mean[2]},moment{pixel.second[0],pixel.second[1],pixel.second[2]};
        const double rays=pixel.mean[3],bound=double(image->info.samples.count)*(2*image->info.settings.max_bounces+1);
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
} // namespace dk::render

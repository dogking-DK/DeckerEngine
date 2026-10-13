#include <dk/render/CloudReferenceRenderer.hpp>
#include <dk/graphics/GraphExecution.hpp>
#include <dk/graphics/ShaderCompiler.hpp>
#include <dk/memory/SmartPtr.hpp>
#include "CloudReferenceInternal.hpp"
#include <algorithm>
#include <cmath>

namespace dk::render {
using namespace graphics;
namespace detail {
struct CloudReferenceState {
    memory::ResourceHandle heap;
    ResourceFactory factory;
    Buffer identity;
    PipelineLayout layout;
    ComputePipeline pipeline;
};
}
namespace {
template<class T> T take(Result<T> r) { if(!r) throw r.error(); return std::move(*r); }
void check(Result<void> r) { if(!r) throw r.error(); }
using Float4=std::array<float,4>;
struct Parameters {
    Float4 geometry,cloud,scales,lighting,sun,irradiance,sky,rayleigh,mie,absorption,air;
    std::array<std::uint32_t,4> counts,flags;
};
struct Ray { Float4 geometry,direction,weather,base,detail,segments; };
struct Pixel { Float4 color,statistics; };
static_assert(sizeof(Parameters)==208 && sizeof(Ray)==96 && sizeof(Pixel)==32);
Float4 pack(const EnvironmentVector& v,double w=0) { return {float(v[0]),float(v[1]),float(v[2]),float(w)}; }
Parameters parameters(const EnvironmentDescription& e,const CloudReferenceSettings& s,std::size_t count) {
    const auto& a=e.atmosphere; const auto& c=e.clouds;
    Parameters p{};
    p.geometry={float(a.ground_radius_m),float(a.atmosphere_height_m),float(c.bottom_height_m),float(c.top_height_m)};
    p.cloud={float(c.coverage),float(c.density),float(c.extinction_per_m),float(s.homogeneous_density)};
    p.scales={float(c.weather_scale_m),float(c.shape_scale_m),float(c.detail_scale_m),float(c.bottom_fade_fraction)};
    p.lighting={float(s.phase_g),float(s.single_scattering_albedo),float(s.multiple_scattering),float(c.top_fade_fraction)};
    p.sun=pack(e.sun.direction); p.irradiance=pack(e.sun.irradiance);
    p.sky={float(s.sky_radiance.x()),float(s.sky_radiance.y()),float(s.sky_radiance.z()),0};
    p.rayleigh=pack(a.rayleigh_scattering_per_m,a.rayleigh_scale_height_m);
    p.mie=pack(a.mie_extinction_per_m,a.mie_scale_height_m);
    p.absorption=pack(a.absorption_extinction_per_m,a.absorption_center_height_m);
    p.air[0]=float(a.absorption_half_width_m);
    p.counts={s.steps,s.sun_steps,s.atmosphere_steps,static_cast<std::uint32_t>(count)};
    p.flags={c.enabled?1u:0u,e.enabled?1u:0u,s.self_shadow?1u:0u,s.homogeneous?1u:0u}; return p;
}
float coordinate(double value,double scale) { const double x=value/scale; return float(x-std::floor(x)); }
Ray pack_ray(const EnvironmentDescription& e,const detail::CloudPreparedRay& p,const CloudReferenceSettings& s) {
    const auto& c=e.clouds; Vec3d origin=p.ray.origin_m-Vec3d{e.atmosphere.planet_center_m.data()};
    origin.x()-=c.wind_velocity_mps[0]*s.time_seconds; origin.z()-=c.wind_velocity_mps[1]*s.time_seconds;
    Ray r{}; r.geometry={float(p.height),float(p.mu),float(p.sun_mu),float(p.cosine)};
    r.direction={float(p.ray.direction.x()),float(p.ray.direction.y()),float(p.ray.direction.z()),float(p.intervals.count)};
    r.weather={coordinate(origin.x(),c.weather_scale_m),coordinate(origin.z(),c.weather_scale_m),0,0};
    for(unsigned i=0;i<3;++i) { r.base[i]=coordinate(origin[i],c.shape_scale_m); r.detail[i]=coordinate(origin[i],c.detail_scale_m); }
    for(unsigned i=0;i<2;++i) { r.segments[2*i]=float(p.intervals.segments[i].begin_m); r.segments[2*i+1]=float(p.intervals.segments[i].end_m); }
    return r;
}
constexpr auto compute=vk::PipelineStageFlagBits2::eComputeShader;
graph::Use use(graph::ResourceId id,vk::PipelineStageFlags2 stage,vk::AccessFlags2 access,
    vk::ImageLayout layout=vk::ImageLayout::eUndefined,bool overwrite=false) {
    return {id,{{stage,access,layout},0,VK_WHOLE_SIZE,{vk::ImageAspectFlagBits::eColor,0,1,0,1},overwrite}};
}
struct Context { detail::CloudReferenceState& state; const CloudTextures& textures; std::uint32_t count; };
Result<void> dispatch(graph::PassContext& pass,void* data) {
    try {
        const auto& c=*static_cast<Context*>(data);
        const std::array writes{BindingWrite{0,0,BufferBinding{take(pass.buffer(1))}},
            BindingWrite{1,0,BufferBinding{take(pass.buffer(2))}},BindingWrite{2,0,BufferBinding{take(pass.buffer(3))}},
            BindingWrite{3,0,ImageBinding{&c.textures.view(CloudField::weather)}},
            BindingWrite{4,0,ImageBinding{&c.textures.view(CloudField::base)}},
            BindingWrite{5,0,ImageBinding{&c.textures.view(CloudField::detail)}},BindingWrite{6,0,SamplerBinding{&c.textures.sampler()}}};
        auto bindings=take(c.state.factory.create_bindings(c.state.layout,0,writes));
        auto encoder=take(pass.compute()); check(encoder.bind_pipeline(c.state.pipeline));
        const std::array sets{&bindings}; check(encoder.bind_sets(sets)); return encoder.dispatch({(c.count+31)/32,1,1});
    } catch(const Error& error) { return std::unexpected(error); }
}
Result<void> copy(graph::PassContext& pass,void* data) {
    return pass.copy_buffer(3,0,static_cast<Context*>(data)->count*sizeof(Pixel));
}
graph::Execution execute(detail::CloudReferenceState& state,SubmissionQueue& queue,const CloudTextures& textures,
    const Parameters& p,std::span<const Ray> rays) {
    auto uniform=take(state.factory.create_buffer({sizeof(p),vk::BufferUsageFlagBits::eUniformBuffer,BufferMemory::upload}));
    auto input=take(state.factory.create_buffer({rays.size_bytes(),vk::BufferUsageFlagBits::eStorageBuffer,BufferMemory::upload}));
    check(uniform.write(0,std::as_bytes(std::span{&p,1}))); check(input.write(0,std::as_bytes(rays)));
    auto g=take(graph::Graph::create(state.heap)); const auto bytes=rays.size()*sizeof(Pixel);
    const auto readback=take(g.declare_buffer("cloud readback",{bytes,vk::BufferUsageFlagBits::eTransferDst,BufferMemory::readback}));
    const auto constants=take(g.declare_buffer("cloud parameters",uniform.description(),graph::Lifetime::external,true));
    const auto ray_data=take(g.declare_buffer("cloud rays",input.description(),graph::Lifetime::external,true));
    const auto output=take(g.declare_buffer("cloud integrals",{bytes,vk::BufferUsageFlagBits::eStorageBuffer|vk::BufferUsageFlagBits::eTransferSrc}));
    std::array<graph::ExternalBinding,5> external{graph::ExternalBinding{1,&uniform},graph::ExternalBinding{2,&input}};
    std::array<graph::Use,6> uses{use(constants,compute,vk::AccessFlagBits2::eUniformRead),use(ray_data,compute,vk::AccessFlagBits2::eShaderStorageRead),
        use(output,compute,vk::AccessFlagBits2::eShaderStorageWrite,vk::ImageLayout::eUndefined,true)};
    for(unsigned i=0;i<3;++i) {
        const auto& img=textures.image(static_cast<CloudField>(i));
        const auto id=take(g.declare_image("cloud field "+std::to_string(i),img.description(),graph::Lifetime::external,true));
        uses[3+i]=use(id,compute,vk::AccessFlagBits2::eShaderSampledRead,vk::ImageLayout::eShaderReadOnlyOptimal); external[2+i]={4+i,nullptr,&img,{}};
    }
    static_cast<void>(take(g.add_pass({"cloud reference",uses})));
    const std::array copies{use(output,vk::PipelineStageFlagBits2::eCopy,vk::AccessFlagBits2::eTransferRead),
        use(readback,vk::PipelineStageFlagBits2::eCopy,vk::AccessFlagBits2::eTransferWrite,vk::ImageLayout::eUndefined,true)};
    static_cast<void>(take(g.add_pass({"cloud readback",copies}))); check(g.mark_output(readback));
    auto plan=take(g.compile()); Context context{state,textures,static_cast<std::uint32_t>(rays.size())};
    const std::array callbacks{graph::PassCallback{0,dispatch,&context},graph::PassCallback{1,copy,&context}};
    const std::array finals{graph::FinalAccess{0,{{vk::PipelineStageFlagBits2::eHost,vk::AccessFlagBits2::eHostRead}}}};
    return take(graph::execute(plan,queue,{external,callbacks,finals}));
}
}
Result<CloudReferenceRenderer> CloudReferenceRenderer::create(memory::ResourceHandle heap,SubmissionQueue& queue,const std::filesystem::path& directory) {
    if(!heap || heap.state()!=memory::ResourceState::open || queue.stats().closed || queue.stats().device_lost)
        return std::unexpected(Error{ErrorCode::invalid_state,"Cloud reference requires open memory and queue"});
    try {
        auto state=memory::make_shared_in<detail::CloudReferenceState>(heap); state->heap=heap; state->factory=queue.resources();
        state->identity=take(state->factory.create_buffer({4}));
        auto compiled=take(compile_shader({directory/"cloud-reference.slang","computeMain",ShaderStage::compute},heap));
        auto shader=take(state->factory.create_shader(compiled)); const std::array shaders{&shader};
        state->layout=take(state->factory.create_pipeline_layout(shaders));
        state->pipeline=take(state->factory.create_compute_pipeline({&shader,&state->layout}));
        return CloudReferenceRenderer{std::move(state)};
    } catch(const Error& error) { return std::unexpected(error); }
    catch(const std::bad_alloc&) { return std::unexpected(Error{ErrorCode::internal_error,"Cloud reference allocation failed"}); }
}
Result<std::vector<CloudIntegral>> CloudReferenceRenderer::integrate(SubmissionQueue& queue,const EnvironmentDescription& input,
    const CloudTextures& textures,std::span<const CloudRay> rays,const CloudReferenceSettings& settings) {
    if(!state_ || state_->heap.state()!=memory::ResourceState::open)
        return std::unexpected(Error{ErrorCode::invalid_state,"Cloud reference owner unavailable"});
    try {
        const auto e=take(detail::validate_cloud_reference(input,settings,rays.size())); const auto& c=e.clouds;
        if(textures.key()!=cloud_field_key(c) || !textures.bytes()) throw Error{ErrorCode::invalid_argument,"Cloud texture key mismatch"};
        if(e.atmosphere.ground_radius_m<1000 || c.weather_scale_m<100 || c.shape_scale_m<100 || c.detail_scale_m<10)
            throw Error{ErrorCode::not_supported,"Cloud reference outside GPU radius or noise scale domain"};
        if(sizeof(Parameters)+rays.size()*(sizeof(Ray)+2*sizeof(Pixel))>settings.max_bytes)
            throw Error{ErrorCode::invalid_state,"Cloud reference byte budget exceeded"};
        std::vector<Ray> packed; packed.reserve(rays.size());
        for(const auto& ray:rays) packed.push_back(pack_ray(e,take(detail::prepare_cloud_ray(e,ray)),settings));
        // Identity and all borrowed images must belong to this queue, even for zero-cloud requests.
        { auto guard=take(queue.begin()); check(guard.retain(state_->identity));
            for(unsigned i=0;i<3;++i) check(guard.retain(textures.image(static_cast<CloudField>(i)))); }
        std::vector<Pixel> pixels(rays.size()); std::vector<CloudIntegral> result(rays.size());
        auto execution=execute(*state_,queue,textures,parameters(e,settings,rays.size()),packed);
        if(!take(queue.wait(execution.submission()))) throw Error{ErrorCode::invalid_state,"Cloud reference wait incomplete"};
        check(take(execution.buffer(0))->read(0,std::as_writable_bytes(std::span{pixels})));
        for(std::size_t i=0;i<pixels.size();++i) {
            const auto& v=pixels[i];
            for(float x:v.color) if(!std::isfinite(x) || x<0) throw Error{ErrorCode::internal_error,"Invalid cloud radiance or transmittance"};
            for(float x:v.statistics) if(!std::isfinite(x) || x<0) throw Error{ErrorCode::internal_error,"Invalid cloud distance or samples"};
            if(v.color[3]>1 || v.statistics[2]>2*settings.steps || v.statistics[1]>rays[i].max_distance_m+2)
                throw Error{ErrorCode::internal_error,"Cloud reference output exceeds bounds"};
            result[i]={{v.color[0],v.color[1],v.color[2]},v.color[3],v.statistics[0],v.statistics[1],static_cast<std::uint32_t>(v.statistics[2])};
        }
        return result;
    } catch(const Error& error) { return std::unexpected(error); }
    catch(const std::bad_alloc&) { return std::unexpected(Error{ErrorCode::internal_error,"Cloud reference allocation failed"}); }
}
} // namespace dk::render

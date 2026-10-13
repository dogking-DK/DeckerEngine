#include <dk/render/CloudShadowRenderer.hpp>
#include <dk/graphics/GraphExecution.hpp>
#include <dk/graphics/ShaderCompiler.hpp>
#include <dk/memory/SmartPtr.hpp>
#include "CloudShadowInternal.hpp"
#include <algorithm>

namespace dk::render {
using namespace graphics;
namespace detail {
struct CloudShadowState {
    EnvironmentDescription environment;
    CloudShadowSettings settings;
    CloudShadowProjection projection;
    std::uint64_t generation=0,last_use=0;
    std::size_t bytes=0,image_index=0;
    graph::Execution execution;
};
struct CloudShadowOwner {
    memory::ResourceHandle heap;
    ResourceFactory factory;
    Buffer identity;
    PipelineLayout layout;
    ComputePipeline pipeline;
    CloudShadowLimits limits;
    std::shared_ptr<CloudShadowState> current;
    std::vector<std::shared_ptr<CloudShadowState>> live;
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
struct Ray { Float4 geometry,weather,base,detail; };
static_assert(sizeof(Parameters)==208 && sizeof(Ray)==64);
float phase(double x,double scale) { x/=scale; return float(x-std::floor(x)); }
Parameters parameters(const EnvironmentDescription& e,const CloudShadowSettings& s) {
    Parameters p{}; const auto& a=e.atmosphere; const auto& c=e.clouds;
    p.geometry={float(a.ground_radius_m),float(a.atmosphere_height_m),float(c.bottom_height_m),float(c.top_height_m)};
    p.cloud={float(c.coverage),float(c.density),float(c.extinction_per_m),float(s.homogeneous_density)};
    p.scales={float(c.weather_scale_m),float(c.shape_scale_m),float(c.detail_scale_m),float(c.bottom_fade_fraction)};
    p.lighting[3]=float(c.top_fade_fraction);
    for(unsigned i=0;i<3;++i) p.sun[i]=float(e.sun.direction[i]);
    p.counts={s.steps,0,0,s.resolution}; p.flags={c.enabled?1u:0u,0,0,s.homogeneous?1u:0u}; return p;
}
constexpr auto compute=vk::PipelineStageFlagBits2::eComputeShader;
graph::Use use(graph::ResourceId id,vk::PipelineStageFlags2 stages,vk::AccessFlags2 access,
    vk::ImageLayout layout=vk::ImageLayout::eUndefined,bool overwrite=false) {
    return {id,{{stages,access,layout},0,VK_WHOLE_SIZE,{vk::ImageAspectFlagBits::eColor,0,1,0,1},overwrite}};
}
struct Context { detail::CloudShadowOwner& owner; const CloudTextures& textures; std::uint32_t side; std::size_t image; };
Result<void> dispatch(graph::PassContext& pass,void* data) {
    try {
        const auto& c=*static_cast<Context*>(data); auto view=take(c.owner.factory.create_view(*take(pass.image(c.image))));
        const std::array writes{BindingWrite{0,0,BufferBinding{take(pass.buffer(0))}},BindingWrite{1,0,BufferBinding{take(pass.buffer(1))}},
            BindingWrite{2,0,ImageBinding{&view,vk::ImageLayout::eGeneral}},BindingWrite{3,0,ImageBinding{&c.textures.view(CloudField::weather)}},
            BindingWrite{4,0,ImageBinding{&c.textures.view(CloudField::base)}},BindingWrite{5,0,ImageBinding{&c.textures.view(CloudField::detail)}},
            BindingWrite{6,0,SamplerBinding{&c.textures.sampler()}}};
        auto bindings=take(c.owner.factory.create_bindings(c.owner.layout,0,writes)); auto encoder=take(pass.compute());
        check(encoder.bind_pipeline(c.owner.pipeline)); const std::array sets{&bindings}; check(encoder.bind_sets(sets));
        return encoder.dispatch({(c.side+7)/8,(c.side+7)/8,1});
    } catch(const Error& e) { return std::unexpected(e); }
}
Result<void> copy(graph::PassContext& pass,void* data) {
    const auto& c=*static_cast<Context*>(data); ImageCopyRegion region; region.width=c.side; region.height=c.side;
    return pass.copy_to_buffer(c.image,2,region);
}
graph::Execution execute(detail::CloudShadowOwner& owner,SubmissionQueue& queue,const CloudTextures& textures,
    detail::CloudShadowState& state,const Parameters& p,std::span<const Ray> rays) {
    auto uniform=take(owner.factory.create_buffer({sizeof(p),vk::BufferUsageFlagBits::eUniformBuffer,BufferMemory::upload}));
    auto input=take(owner.factory.create_buffer({rays.size_bytes(),vk::BufferUsageFlagBits::eStorageBuffer,BufferMemory::upload}));
    check(uniform.write(0,std::as_bytes(std::span{&p,1}))); check(input.write(0,std::as_bytes(rays)));
    auto g=take(graph::Graph::create(owner.heap));
    const auto constants=take(g.declare_buffer("cloud shadow parameters",uniform.description(),graph::Lifetime::external,true));
    const auto ray_data=take(g.declare_buffer("cloud shadow rays",input.description(),graph::Lifetime::external,true));
    graph::ResourceId readback{};
    if(state.settings.diagnostics) readback=take(g.declare_buffer("cloud shadow readback",{rays.size()*4,vk::BufferUsageFlagBits::eTransferDst,BufferMemory::readback}));
    const auto side=state.settings.resolution; state.image_index=g.counts().buffers;
    ImageDesc desc{side,side,vk::Format::eR32Sfloat,vk::ImageUsageFlagBits::eStorage|vk::ImageUsageFlagBits::eSampled|vk::ImageUsageFlagBits::eTransferSrc}; desc.require_linear_filter=true;
    const auto image=take(g.declare_image("sunward cloud column transmittance",desc));
    std::array<graph::ExternalBinding,5> external{graph::ExternalBinding{0,&uniform},graph::ExternalBinding{1,&input}};
    std::array<graph::Use,6> uses{use(constants,compute,vk::AccessFlagBits2::eUniformRead),use(ray_data,compute,vk::AccessFlagBits2::eShaderStorageRead),
        use(image,compute,vk::AccessFlagBits2::eShaderStorageWrite,vk::ImageLayout::eGeneral,true)};
    for(unsigned i=0;i<3;++i) {
        const auto& field=textures.image(static_cast<CloudField>(i)); const auto id=take(g.declare_image("cloud shadow field "+std::to_string(i),field.description(),graph::Lifetime::external,true));
        external[2+i]={state.image_index+1+i,nullptr,&field,{}};
        uses[3+i]=use(id,compute,vk::AccessFlagBits2::eShaderSampledRead,vk::ImageLayout::eShaderReadOnlyOptimal);
    }
    static_cast<void>(take(g.add_pass({"cloud shadow column integration",uses}))); check(g.mark_output(image));
    if(state.settings.diagnostics) {
        const std::array copies{use(image,vk::PipelineStageFlagBits2::eCopy,vk::AccessFlagBits2::eTransferRead,vk::ImageLayout::eTransferSrcOptimal),
            use(readback,vk::PipelineStageFlagBits2::eCopy,vk::AccessFlagBits2::eTransferWrite,vk::ImageLayout::eUndefined,true)};
        static_cast<void>(take(g.add_pass({"cloud shadow readback",copies}))); check(g.mark_output(readback));
    }
    auto plan=take(g.compile()); Context context{owner,textures,side,state.image_index};
    const std::array callbacks{graph::PassCallback{0,dispatch,&context},graph::PassCallback{1,copy,&context}};
    const std::array finals{graph::FinalAccess{state.image_index,{{vk::PipelineStageFlagBits2::eFragmentShader,vk::AccessFlagBits2::eShaderSampledRead,vk::ImageLayout::eShaderReadOnlyOptimal}}},
        graph::FinalAccess{2,{{vk::PipelineStageFlagBits2::eHost,vk::AccessFlagBits2::eHostRead}}}};
    return take(graph::execute(plan,queue,{external,std::span{callbacks}.first(state.settings.diagnostics?2:1),std::span{finals}.first(state.settings.diagnostics?2:1)}));
}
Error missing() { return {ErrorCode::invalid_state,"Cloud shadow snapshot unavailable"}; }
}
Result<CloudShadowRenderer> CloudShadowRenderer::create(memory::ResourceHandle heap,SubmissionQueue& queue,const std::filesystem::path& directory,CloudShadowLimits limits) {
    if(!heap || heap.state()!=memory::ResourceState::open || queue.stats().closed || queue.stats().device_lost || !limits.generations || limits.generations>3 || !limits.bytes || limits.bytes>64ULL*1024*1024)
        return std::unexpected(Error{ErrorCode::invalid_argument,"Cloud shadow owner requires open memory/queue and bounded limits"});
    try {
        auto state=memory::make_shared_in<detail::CloudShadowOwner>(heap); state->heap=heap; state->factory=queue.resources(); state->limits=limits;
        state->live.reserve(limits.generations); state->identity=take(state->factory.create_buffer({4}));
        auto code=take(compile_shader({directory/"cloud-shadow.slang","computeMain",ShaderStage::compute},heap));
        auto shader=take(state->factory.create_shader(code)); const std::array shaders{&shader};
        state->layout=take(state->factory.create_pipeline_layout(shaders)); state->pipeline=take(state->factory.create_compute_pipeline({&shader,&state->layout}));
        return CloudShadowRenderer{std::move(state)};
    } catch(const Error& e) { return std::unexpected(e); }
    catch(const std::bad_alloc&) { return std::unexpected(Error{ErrorCode::internal_error,"Cloud shadow owner allocation failed"}); }
}
Result<CloudShadowMap> CloudShadowRenderer::prepare(SubmissionQueue& queue,const EnvironmentDescription& input,const CloudTextures& textures,const CloudShadowSettings& settings) {
    if(!state_ || state_->heap.state()!=memory::ResourceState::open) return std::unexpected(missing());
    try {
        const auto e=take(normalize_environment(input)); const auto projection=take(cloud_shadow_projection(e,settings));
        if(textures.key()!=cloud_field_key(e.clouds) || !textures.bytes()) throw Error{ErrorCode::invalid_argument,"Cloud shadow texture key mismatch"};
        { auto guard=take(queue.begin()); check(guard.retain(state_->identity)); for(unsigned i=0;i<3;++i) check(guard.retain(textures.image(static_cast<CloudField>(i)))); }
        check(queue.poll());
        std::erase_if(state_->live,[&](const auto& old){ return old!=state_->current && old.use_count()==1 && old->last_use<=queue.stats().completed; });
        if(state_->current && state_->current->environment==e && state_->current->settings==settings) return CloudShadowMap{state_->current};
        std::size_t bytes=cloud_shadow_bytes(settings); for(const auto& old:state_->live) bytes+=old->bytes;
        if(state_->live.size()>=state_->limits.generations || bytes>state_->limits.bytes || (state_->current && state_->current->generation==UINT64_MAX))
            throw Error{ErrorCode::invalid_state,"Cloud shadow live generation or byte budget exceeded"};
        auto candidate=memory::make_shared_in<detail::CloudShadowState>(state_->heap);
        candidate->environment=e; candidate->settings=settings; candidate->projection=projection; candidate->bytes=cloud_shadow_bytes(settings);
        candidate->generation=state_->current ? state_->current->generation+1 : 1;
        std::vector<Ray> rays(std::size_t{settings.resolution}*settings.resolution); const auto& c=e.clouds;
        const Vec3d center{e.atmosphere.planet_center_m.data()},wind{c.wind_velocity_mps[0]*settings.time_seconds,0,c.wind_velocity_mps[1]*settings.time_seconds};
        for(unsigned y=0;y<settings.resolution;++y) for(unsigned x=0;x<settings.resolution;++x) {
            const auto ray=detail::shadow_ray(e,settings,projection,x,y); const Vec3d radial=ray.origin_m-center,origin=radial-wind;
            auto& r=rays[std::size_t{y}*settings.resolution+x]; r.geometry={float(c.bottom_height_m),float(radial.normalized().dot(projection.sun)),float(ray.max_distance_m),0};
            r.weather={phase(origin.x(),c.weather_scale_m),phase(origin.z(),c.weather_scale_m),0,0};
            for(unsigned i=0;i<3;++i) { r.base[i]=phase(origin[i],c.shape_scale_m); r.detail[i]=phase(origin[i],c.detail_scale_m); }
        }
        candidate->execution=execute(*state_,queue,textures,*candidate,parameters(e,settings),rays);
        candidate->last_use=candidate->execution.submission().value();
        state_->live.push_back(candidate); state_->current=std::move(candidate); return CloudShadowMap{state_->current};
    } catch(const Error& e) { return std::unexpected(e); }
    catch(const std::bad_alloc&) { return std::unexpected(Error{ErrorCode::internal_error,"Cloud shadow allocation failed; current snapshot unchanged"}); }
}
CloudShadowMap CloudShadowRenderer::current() const { return state_ ? CloudShadowMap{state_->current} : CloudShadowMap{}; }
std::uint64_t CloudShadowMap::generation() const noexcept { return state_ ? state_->generation : 0; }
Submission CloudShadowMap::submission() const noexcept { return state_ ? state_->execution.submission() : Submission{}; }
Result<EnvironmentDescription> CloudShadowMap::environment() const { if(!state_) return std::unexpected(missing()); return state_->environment; }
Result<CloudShadowSettings> CloudShadowMap::settings() const { if(!state_) return std::unexpected(missing()); return state_->settings; }
Result<CloudShadowProjection> CloudShadowMap::projection() const { if(!state_) return std::unexpected(missing()); return state_->projection; }
Result<const Image*> CloudShadowMap::image() const { if(!state_) return std::unexpected(missing()); return state_->execution.image(state_->image_index); }
void CloudShadowMap::used_by(Submission submission) const noexcept { if(state_) state_->last_use=std::max(state_->last_use,submission.value()); }
Result<void> CloudShadowMap::read_transmittance(std::span<float> destination) const {
    if(!state_ || !state_->settings.diagnostics) return std::unexpected(missing());
    if(destination.size()!=std::size_t{state_->settings.resolution}*state_->settings.resolution)
        return std::unexpected(Error{ErrorCode::invalid_argument,"Cloud shadow readback size mismatch"});
    auto buffer=state_->execution.buffer(2); if(!buffer) return std::unexpected(buffer.error());
    return (*buffer)->read(0,std::as_writable_bytes(destination));
}
} // namespace dk::render

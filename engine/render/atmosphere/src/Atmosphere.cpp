#include <dk/render/Atmosphere.hpp>
#include <dk/graphics/ShaderCompiler.hpp>
#include <dk/memory/SmartPtr.hpp>
#include <dk/profiling/Profiler.hpp>
#include <algorithm>
#include <cmath>
#include <numbers>

namespace dk::render {
using namespace graphics;
namespace detail {
using Pixel=std::array<float,4>;
struct AtmosphereTable {
    explicit AtmosphereTable(memory::ResourceHandle heap) : pixels(0,memory::Allocator<Pixel>{heap}) {}
    Image image;
    Vector<Pixel> pixels;
};
struct SkyState {
    EnvironmentDescription environment;
    Vec3d position,up;
    double height=0,sun_mu=0;
    bool multiple=true;
    AtmosphereGenerations generations;
    std::array<std::shared_ptr<const AtmosphereTable>,3> tables;
};
struct AtmosphereProgram { PipelineLayout layout; ComputePipeline pipeline; };
struct AtmosphereState {
    memory::ResourceHandle heap;
    ResourceFactory factory;
    Buffer identity;
    Sampler sampler;
    std::array<AtmosphereProgram,4> programs;
    std::shared_ptr<const SkyState> current;
};
struct SkyFrameState { std::uint32_t width=0,height=0; graph::Execution execution; };
}
namespace {
Error allocation_error() { return {ErrorCode::internal_error,"atmosphere memory allocation failed"}; }
template<class T> T take(Result<T> result) { if(!result) throw std::move(result.error()); return std::move(*result); }
void check(Result<void> result) { if(!result) throw std::move(result.error()); }
constexpr auto range=vk::ImageSubresourceRange{vk::ImageAspectFlagBits::eColor,0,1,0,1};
constexpr auto compute=vk::PipelineStageFlagBits2::eComputeShader;
constexpr auto sampled=vk::AccessFlagBits2::eShaderSampledRead;
using Float4=std::array<float,4>;
struct Parameters {
    Float4 geometry,rayleigh,mie,extinction,absorption,ground,sun,forward,right,up,sun_direction;
    std::array<std::uint32_t,4> frame;
};
static_assert(sizeof(Parameters)==192);
Vec3d vector(const EnvironmentVector& v) { return {v[0],v[1],v[2]}; }
Float4 pack(const EnvironmentVector& v,double w,double scale=1) {
    return {float(v[0]*scale),float(v[1]*scale),float(v[2]*scale),float(w)};
}
Parameters parameters(const detail::SkyState& sky) {
    const auto& a=sky.environment.atmosphere; const auto& sun=sky.environment.sun;
    Parameters p{};
    p.geometry={float(a.ground_radius_m*.001),float(a.atmosphere_height_m*.001),float(sky.height*.001),float(sky.sun_mu)};
    p.rayleigh=pack(a.rayleigh_scattering_per_m,a.rayleigh_scale_height_m*.001,1000);
    p.mie=pack(a.mie_scattering_per_m,a.mie_scale_height_m*.001,1000);
    p.extinction=pack(a.mie_extinction_per_m,a.mie_anisotropy,1000);
    p.absorption=pack(a.absorption_extinction_per_m,a.absorption_center_height_m*.001,1000);
    p.ground=pack(a.ground_albedo,a.absorption_half_width_m*.001);
    const double sine=std::sin(sun.angular_radius_rad),half_sine=std::sin(sun.angular_radius_rad*.5);
    p.sun=pack(sun.irradiance,1/(std::numbers::pi*sine*sine));
    p.up[3]=sky.multiple ? 1.f : 0.f;
    p.sun_direction={float(std::sqrt(std::max(0.,1-sky.sun_mu*sky.sun_mu))),float(sky.sun_mu),0,float(4*half_sine*half_sine)};
    return p;
}
void guard(detail::AtmosphereState& state,SubmissionQueue& queue) {
    if(state.heap.state()!=memory::ResourceState::open || queue.stats().closed || queue.stats().device_lost)
        throw Error{ErrorCode::invalid_state,"atmosphere memory or queue is unavailable"};
    // Also validates queue identity on cache hits, without retaining a raw queue pointer.
    auto batch=take(queue.begin()); check(batch.retain(state.identity));
}
AtmosphereParameters transmittance_key(AtmosphereParameters a) {
    a.planet_center_m={}; a.mie_scattering_per_m={}; a.mie_anisotropy=0; a.ground_albedo={}; return a;
}
bool same_ms(const AtmosphereParameters& a,const AtmosphereParameters& b) {
    return transmittance_key(a)==transmittance_key(b) && a.mie_scattering_per_m==b.mie_scattering_per_m && a.ground_albedo==b.ground_albedo;
}
graph::Use use(graph::ResourceId id,vk::PipelineStageFlags2 stage,vk::AccessFlags2 access,
    vk::ImageLayout layout=vk::ImageLayout::eUndefined,bool overwrite=false) {
    return {id,{{stage,access,layout},0,VK_WHOLE_SIZE,range,overwrite}};
}
struct Context { detail::AtmosphereState& state; unsigned program; std::uint32_t width,height; };
Result<void> dispatch(graph::PassContext& pass,void* data) {
    try {
        auto& c=*static_cast<Context*>(data); auto& program=c.state.programs[c.program];
        auto out=take(c.state.factory.create_view(*take(pass.image(2))));
        std::array<ImageView,3> inputs;
        std::array<BindingWrite,6> writes{BindingWrite{0,0,BufferBinding{take(pass.buffer(1))}},BindingWrite{1,0,ImageBinding{&out,vk::ImageLayout::eGeneral}}};
        std::size_t count=2;
        if(c.program>0) writes[count++]={2,0,SamplerBinding{&c.state.sampler}};
        for(unsigned i=0;i<c.program;++i) {
            inputs[i]=take(c.state.factory.create_view(*take(pass.image(3+i))));
            writes[count++]={3+i,0,ImageBinding{&inputs[i]}};
        }
        auto bindings=take(c.state.factory.create_bindings(program.layout,0,std::span{writes}.first(count)));
        auto encoder=take(pass.compute()); check(encoder.bind_pipeline(program.pipeline));
        const std::array sets{&bindings}; check(encoder.bind_sets(sets));
        return encoder.dispatch({(c.width+7)/8,(c.height+7)/8,1});
    } catch(Error& error) { return std::unexpected(std::move(error)); }
}
Result<void> copy(graph::PassContext& pass,void* data) {
    const auto& c=*static_cast<Context*>(data);
    return pass.copy_to_buffer(2,0,{0,0,0,0,c.width,c.height});
}
graph::Execution execute(detail::AtmosphereState& state,SubmissionQueue& queue,const Parameters& parameters,
    unsigned program,std::uint32_t width,std::uint32_t height,const std::array<std::shared_ptr<const detail::AtmosphereTable>,3>& tables) {
    auto uniform=take(state.factory.create_buffer({sizeof(Parameters),vk::BufferUsageFlagBits::eUniformBuffer,BufferMemory::upload}));
    check(uniform.write(0,std::as_bytes(std::span{&parameters,1})));
    auto graph=take(graph::Graph::create(state.heap));
    const auto readback=take(graph.declare_buffer("atmosphere readback",{vk::DeviceSize{width}*height*16,vk::BufferUsageFlagBits::eTransferDst,BufferMemory::readback}));
    const auto constants=take(graph.declare_buffer("atmosphere parameters",uniform.description(),graph::Lifetime::external,true));
    ImageDesc desc{width,height,vk::Format::eR32G32B32A32Sfloat,vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferSrc};
    desc.require_linear_filter=true;
    const auto output=take(graph.declare_image("atmosphere output",desc));
    std::array<graph::ExternalBinding,4> external{graph::ExternalBinding{1,&uniform}};
    std::array<graph::Use,5> uses{use(constants,compute,vk::AccessFlagBits2::eUniformRead),
        use(output,compute,vk::AccessFlagBits2::eShaderStorageWrite,vk::ImageLayout::eGeneral,true)};
    for(unsigned i=0;i<program;++i) {
        const auto& image=tables[i]->image;
        const auto id=take(graph.declare_image("atmosphere input "+std::to_string(i),image.description(),graph::Lifetime::external,true));
        uses[2+i]=use(id,compute,sampled,vk::ImageLayout::eShaderReadOnlyOptimal);
        external[1+i]={3+i,nullptr,&image,{}};
    }
    static_cast<void>(take(graph.add_pass({"atmosphere compute",std::span{uses}.first(2+program)})));
    const std::array copies{use(output,vk::PipelineStageFlagBits2::eCopy,vk::AccessFlagBits2::eTransferRead,vk::ImageLayout::eTransferSrcOptimal),
        use(readback,vk::PipelineStageFlagBits2::eCopy,vk::AccessFlagBits2::eTransferWrite,vk::ImageLayout::eUndefined,true)};
    static_cast<void>(take(graph.add_pass({"atmosphere readback",copies})));
    check(graph.mark_output(readback)); check(graph.mark_output(output));
    auto plan=take(graph.compile()); Context context{state,program,width,height};
    const std::array callbacks{graph::PassCallback{0,dispatch,&context},graph::PassCallback{1,copy,&context}};
    const std::array finals{graph::FinalAccess{0,{{vk::PipelineStageFlagBits2::eHost,vk::AccessFlagBits2::eHostRead}}},
        graph::FinalAccess{2,{{compute,sampled,vk::ImageLayout::eShaderReadOnlyOptimal}}}};
    return take(graph::execute(plan,queue,{std::span{external}.first(1+program),callbacks,finals}));
}
std::shared_ptr<const detail::AtmosphereTable> build(detail::AtmosphereState& state,SubmissionQueue& queue,const detail::SkyState& sky,unsigned index) {
    constexpr std::array<std::uint32_t,3> widths{256,32,192},heights{64,32,108};
    auto table=memory::make_shared_in<detail::AtmosphereTable>(state.heap,state.heap);
    table->pixels.resize(std::size_t{widths[index]}*heights[index]); // Reserve before submitting.
    auto result=execute(state,queue,parameters(sky),index,widths[index],heights[index],sky.tables);
    if(!take(queue.wait(result.submission()))) throw Error{ErrorCode::invalid_state,"atmosphere LUT did not complete"};
    check(take(result.buffer(0))->read(0,std::as_writable_bytes(std::span{table->pixels})));
    for(const auto& pixel:table->pixels) {
        for(unsigned c=0;c<4;++c) if(!std::isfinite(pixel[c]) || pixel[c]<0 || (index==0 && pixel[c]>1))
            throw Error{ErrorCode::not_supported,"atmosphere LUT has nonfinite, negative or out-of-range values"};
        if(index==1 && pixel[3]>=1) throw Error{ErrorCode::not_supported,"atmosphere multiple scattering closure does not converge"};
    }
    table->image=take(result.image(2))->share(); return table;
}
}
Result<AtmosphereRenderer> AtmosphereRenderer::create(memory::ResourceHandle heap,SubmissionQueue& queue,const std::filesystem::path& directory) {
    if(!heap || heap.state()!=memory::ResourceState::open || queue.stats().closed || queue.stats().device_lost)
        return std::unexpected(Error{ErrorCode::invalid_state,"atmosphere renderer requires open memory and queue"});
    try {
        auto state=memory::make_shared_in<detail::AtmosphereState>(heap); state->heap=heap; state->factory=queue.resources();
        state->identity=take(state->factory.create_buffer({4}));
        SamplerDesc sampler; sampler.address_u=sampler.address_v=sampler.address_w=vk::SamplerAddressMode::eClampToEdge;
        state->sampler=take(state->factory.create_sampler(sampler));
        for(unsigned i=0;i<4;++i) {
            const auto value=std::to_string(i); const std::array defines{ShaderDefine{"DK_STAGE",value}};
            auto compiled=take(compile_shader({directory/"atmosphere.slang","computeMain",ShaderStage::compute,{},defines},heap));
            auto shader=take(state->factory.create_shader(compiled)); const std::array shaders{&shader};
            state->programs[i].layout=take(state->factory.create_pipeline_layout(shaders));
            state->programs[i].pipeline=take(state->factory.create_compute_pipeline({&shader,&state->programs[i].layout}));
        }
        return AtmosphereRenderer{std::move(state)};
    } catch(Error& error) { return std::unexpected(std::move(error)); }
    catch(const std::bad_alloc&) { return std::unexpected(allocation_error()); }
}
Result<AtmosphereSky> AtmosphereRenderer::prepare(SubmissionQueue& queue,const EnvironmentDescription& input,const Vec3d& position,bool multiple) {
    DK_PROFILE_ZONE("atmosphere.prepare");
    if(!state_) return std::unexpected(Error{ErrorCode::invalid_state,"empty atmosphere renderer"});
    try {
        guard(*state_,queue); const auto environment=take(normalize_environment(input)); const auto& a=environment.atmosphere;
        const auto h=take(atmosphere_height_m(a,{position.x(),position.y(),position.z()}));
        if(a.ground_radius_m<100000 || a.ground_radius_m>100000000 || a.atmosphere_height_m<1000 || a.atmosphere_height_m>a.ground_radius_m*.1 ||
            a.rayleigh_scale_height_m<100 || a.mie_scale_height_m<100 || a.absorption_half_width_m<100 || h<0 || h>2*a.atmosphere_height_m)
            throw Error{ErrorCode::not_supported,"atmosphere is outside the supported GPU radius, density or observer domain"};
        auto next=memory::make_shared_in<detail::SkyState>(state_->heap);
        next->environment=environment; next->position=position; next->height=h; next->multiple=multiple;
        next->up=(position-vector(a.planet_center_m)).normalized();
        next->sun_mu=std::clamp(next->up.dot(vector(environment.sun.direction)),-1.0,1.0);
        const auto& previous=state_->current;
        if(previous) { next->tables=previous->tables; next->generations=previous->generations; }
        const bool trans=!previous || transmittance_key(a)!=transmittance_key(previous->environment.atmosphere);
        const bool ms=trans || !same_ms(a,previous->environment.atmosphere);
        const bool sky=ms || a.mie_anisotropy!=previous->environment.atmosphere.mie_anisotropy || h!=previous->height ||
            next->sun_mu!=previous->sun_mu || multiple!=previous->multiple;
        if(trans) { next->tables[0]=build(*state_,queue,*next,0); ++next->generations.transmittance; }
        if(ms) { next->tables[1]=build(*state_,queue,*next,1); ++next->generations.multiple_scattering; }
        if(sky) { next->tables[2]=build(*state_,queue,*next,2); ++next->generations.sky_view; }
        // All allocations, execution and numerical checks precede this noexcept publication.
        state_->current=next; return AtmosphereSky{std::move(next)};
    } catch(Error& error) { return std::unexpected(std::move(error)); }
    catch(const std::bad_alloc&) { return std::unexpected(allocation_error()); }
}
Result<SkyFrame> AtmosphereRenderer::render_sky(SubmissionQueue& queue,const AtmosphereSky& sky,const SkyCamera& camera) {
    DK_PROFILE_ZONE("atmosphere.render_sky");
    if(!state_ || !sky) return std::unexpected(Error{ErrorCode::invalid_state,"empty atmosphere renderer or sky"});
    if(!camera.forward.allFinite() || !camera.up.allFinite() || !std::isfinite(camera.vertical_fov_rad) || camera.vertical_fov_rad<=0 ||
        camera.vertical_fov_rad>=std::numbers::pi || !camera.width || !camera.height || camera.width>4096 || camera.height>4096 ||
        (camera.reference && std::uint64_t{camera.width}*camera.height>4096))
        return std::unexpected(Error{ErrorCode::invalid_argument,"invalid sky camera or extent (reference limited to 4096 pixels)"});
    try {
        guard(*state_,queue);
        const double fl=camera.forward.stableNorm(),ul=camera.up.stableNorm();
        if(!std::isfinite(fl) || !std::isfinite(ul) || fl==0 || ul==0) throw Error{ErrorCode::invalid_argument,"invalid sky camera basis"};
        const Vec3d forward=camera.forward/fl; Vec3d right=forward.cross(camera.up/ul);
        if(right.norm()<1e-8) throw Error{ErrorCode::invalid_argument,"degenerate sky camera basis"};
        right.normalize(); const Vec3d up=right.cross(forward);
        const auto& s=*sky.state_; Vec3d x=vector(s.environment.sun.direction)-s.up*s.sun_mu;
        if(x.norm()<1e-8) x=s.up.unitOrthogonal(); else x.normalize();
        const Vec3d z=x.cross(s.up);
        auto p=parameters(s);
        const auto local=[&](const Vec3d& v,double w) -> Float4 { return {float(v.dot(x)),float(v.dot(s.up)),float(v.dot(z)),float(w)}; };
        p.forward=local(forward,std::tan(camera.vertical_fov_rad*.5));
        p.right=local(right,double(camera.width)/camera.height); p.up=local(up,s.multiple ? 1 : 0);
        p.frame={camera.width,camera.height,camera.reference ? 1u : 0u,(camera.sun_disk ? 1u : 0u) | (s.environment.enabled ? 2u : 0u)};
        auto frame=memory::make_shared_in<detail::SkyFrameState>(state_->heap); frame->width=camera.width; frame->height=camera.height;
        frame->execution=execute(*state_,queue,p,3,camera.width,camera.height,s.tables);
        return SkyFrame{std::move(frame)};
    } catch(Error& error) { return std::unexpected(std::move(error)); }
    catch(const std::bad_alloc&) { return std::unexpected(allocation_error()); }
}
AtmosphereGenerations AtmosphereRenderer::generations() const noexcept { return state_ && state_->current ? state_->current->generations : AtmosphereGenerations{}; }
AtmosphereGenerations AtmosphereSky::generations() const noexcept { return state_ ? state_->generations : AtmosphereGenerations{}; }
Result<AtmosphereLutView> AtmosphereSky::lut(AtmosphereLut index) const {
    if(!state_ || unsigned(index)>=3) return std::unexpected(Error{ErrorCode::invalid_argument,"invalid atmosphere LUT"});
    const auto& table=*state_->tables[unsigned(index)]; const auto desc=table.image.description();
    return AtmosphereLutView{desc.width,desc.height,table.pixels};
}
Result<EnvironmentDescription> AtmosphereSky::environment() const {
    if(!state_) return std::unexpected(Error{ErrorCode::invalid_state,"empty atmosphere sky"});
    return state_->environment;
}
Submission SkyFrame::submission() const noexcept { return state_ ? state_->execution.submission() : Submission{}; }
Result<Vec3d> AtmosphereSky::position_m() const {
    if(!state_) return std::unexpected(Error{ErrorCode::invalid_state,"empty atmosphere sky"});
    return state_->position;
}
Result<const Image*> AtmosphereSky::image(AtmosphereLut index) const {
    if(!state_ || unsigned(index)>=3) return std::unexpected(Error{ErrorCode::invalid_argument,"invalid atmosphere LUT"});
    return &state_->tables[unsigned(index)]->image;
}
Result<bool> SkyFrame::wait(SubmissionQueue& queue,std::uint64_t timeout) const {
    if(!state_) return std::unexpected(Error{ErrorCode::invalid_state,"empty sky frame"});
    return queue.wait(submission(),timeout);
}
Result<void> SkyFrame::read_hdr(std::span<std::array<float,4>> destination) const {
    if(!state_) return std::unexpected(Error{ErrorCode::invalid_state,"empty sky frame"});
    if(destination.size()!=std::size_t{state_->width}*state_->height) return std::unexpected(Error{ErrorCode::invalid_argument,"sky readback extent differs"});
    auto buffer=state_->execution.buffer(0); if(!buffer) return std::unexpected(buffer.error());
    return (*buffer)->read(0,std::as_writable_bytes(destination));
}
Result<const Image*> SkyFrame::color() const {
    if(!state_) return std::unexpected(Error{ErrorCode::invalid_state,"empty sky frame"});
    return state_->execution.image(2);
}
} // namespace dk::render

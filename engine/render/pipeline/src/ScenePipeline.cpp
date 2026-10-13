#include "PipelinePolicy.hpp"
#include <dk/graphics/GraphExecution.hpp>
#include <dk/graphics/GraphDiagnostics.hpp>
#include <dk/graphics/ShaderCompiler.hpp>
#include <dk/memory/SmartPtr.hpp>
#include <dk/profiling/Profiler.hpp>
#include <algorithm>
#ifdef DK_RENDER_SUNLIGHT
#include <dk/render/Atmosphere.hpp>
#endif
#include <new>
#include <numbers>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace dk::render {
using namespace graphics;
namespace detail {
struct PipelineState {
    memory::ResourceHandle heap;
    ResourceFactory factory;
    PipelineLayout depth_layout, mask_layout, opaque_layout, tone_layout;
    GraphicsPipeline depth, mask, opaque, tone;
    Sampler white_sampler, sky_sampler;
    PipelineLayout sunlight_depth_layout, sunlight_layout;
    GraphicsPipeline sunlight_depth, sunlight;
    struct Program { PipelineLayout layout; ComputePipeline pipeline; };
    std::array<Program,2> aerial;
    Sampler aerial_sampler;
    std::filesystem::path directory;
#ifdef DK_RENDER_SUNLIGHT
    std::optional<AtmosphereRenderer> atmosphere;
#endif
};
struct FrameState {
    FrameInfo info;
    std::size_t color = 0, linear_readback = 0;
    graph::Execution execution;
    graph::PlanReport report;
};
}
namespace {
template<class T> T take(Result<T> result) { if (!result) throw std::move(result.error()); return std::move(*result); }
void check(Result<void> result) { if (!result) throw std::move(result.error()); }
constexpr unsigned aerial_side=64,aerial_depth=128;
constexpr auto color_range = vk::ImageSubresourceRange{vk::ImageAspectFlagBits::eColor,0,1,0,1};
constexpr auto depth_range = vk::ImageSubresourceRange{vk::ImageAspectFlagBits::eDepth,0,1,0,1};
constexpr auto depth_stages = vk::PipelineStageFlagBits2::eEarlyFragmentTests | vk::PipelineStageFlagBits2::eLateFragmentTests;
constexpr auto depth_access = vk::AccessFlagBits2::eDepthStencilAttachmentRead | vk::AccessFlagBits2::eDepthStencilAttachmentWrite;
constexpr auto color_access = vk::AccessFlagBits2::eColorAttachmentRead | vk::AccessFlagBits2::eColorAttachmentWrite;
Error allocation_error() { return {ErrorCode::internal_error,"scene pipeline allocation failed; no frame submitted"}; }
struct Draw {
    GpuAsset owner;
    const GpuPrimitive* primitive = nullptr;
    const GpuTexture* texture = nullptr;
    bool masked = false;
    std::array<float,24> push{};
    std::size_t vertices = 0, indices = 0, lighting_index = 0;
    Buffer lighting;
    Transformd world;
    bool double_sided=false;
};
struct Inputs {
    explicit Inputs(memory::ResourceHandle heap)
        : draws(0,memory::Allocator<Draw>{heap}), buffers(0,memory::Allocator<const Buffer*>{heap}),
          textures(0,memory::Allocator<const Image*>{heap}) {}
    Vector<Draw> draws;
    Vector<const Buffer*> buffers;
    Vector<const Image*> textures;
    const Image* transmittance=nullptr;
    const Image* sky=nullptr;
};
void resolve(Inputs& result,const RenderView& view,const GpuAssets& assets) {
    for (std::size_t i=0;i<view.scene().entities().size();++i) {
        const auto& entity = view.scene().entities()[i];
        for (const auto& reference : take(view.scene().assets(i))) {
            if (reference.kind != AssetKind::mesh) continue;
            auto found = assets.find(reference.id);
            if (!found) throw found.error().with_context("entity " + entity.id.to_string() + " mesh " + reference.id.to_string());
            const auto rows = take(detail::clip_rows(view,entity.world));
            for (const auto& primitive : found->mesh().primitives) {
                Draw draw; draw.owner = *found; draw.primitive = &primitive;
                draw.world=entity.world;
                std::copy(rows.begin(),rows.end(),draw.push.begin());
                const MaterialData* material = nullptr;
                if (primitive.material) {
                    auto m = std::ranges::find(found->materials(),*primitive.material,&MaterialData::id);
                    if (m == found->materials().end()) throw Error{ErrorCode::invalid_state,"GPU material reference missing"};
                    material = &*m;
                    draw.double_sided=material->double_sided;
                    if (material->alpha_mode == AlphaMode::blend)
                        throw Error{ErrorCode::not_supported,"scene pipeline supports opaque and alpha mask materials only"};
                    if (material->base_color_texture) {
                        auto t = std::ranges::find(found->textures(),*material->base_color_texture,&GpuTexture::id);
                        if (t == found->textures().end()) throw Error{ErrorCode::invalid_state,"GPU texture reference missing"};
                        draw.texture = &*t;
                        if (std::ranges::find(result.textures,&t->image) == result.textures.end()) result.textures.push_back(&t->image);
                    }
                }
                draw.masked = material && material->alpha_mode == AlphaMode::mask;
                draw.push[23] = draw.masked ? material->alpha_cutoff : -1;
                for (std::size_t c=0;c<4;++c) draw.push[16+c] = material ? material->base_color[static_cast<int>(c)] : 1;
                for (std::size_t c=0;c<3;++c) draw.push[20+c] = material ? material->emissive[static_cast<int>(c)] : 0;
                const auto buffer_index = [&](const Buffer& buffer) {
                    auto it = std::ranges::find(result.buffers,&buffer);
                    if (it != result.buffers.end()) return static_cast<std::size_t>(it-result.buffers.begin())+1;
                    result.buffers.push_back(&buffer); return result.buffers.size(); // Readback is buffer 0.
                };
                draw.vertices = buffer_index(primitive.vertices); draw.indices = buffer_index(primitive.indices);
                result.draws.push_back(std::move(draw));
            }
        }
    }
}
#ifdef DK_RENDER_SUNLIGHT
struct AerialParameters {
    std::array<float,4> geometry,rayleigh,mie,extinction,absorption,ground,sun,forward,right,up,sun_direction;
    std::array<std::uint32_t,4> frame;
    std::array<float,4> depth;
};
static_assert(sizeof(AerialParameters)==208);
Buffer prepare_aerial(detail::PipelineState& state,const RenderView& view,const AtmosphereSky& sky,const AtmosphereSettings& settings) {
    const auto projection=take(detail::perspective_depth(view.description()));
    const auto environment=take(sky.environment());
    const Vec3d anchor=take(sky.position_m());
    if(environment!=view.scene().environment() || anchor!=view.description().camera_world.matrix().topRightCorner<3,1>())
        throw Error{ErrorCode::invalid_argument,"atmosphere snapshot must match scene environment and camera position"};
    if(settings.reference && std::uint64_t(view.description().width)*view.description().height>4096)
        throw Error{ErrorCode::invalid_argument,"atmosphere reference is limited to 4096 pixels"};
    const auto& a=environment.atmosphere; const auto& sun=environment.sun;
    const Vec3d center{a.planet_center_m[0],a.planet_center_m[1],a.planet_center_m[2]},up=(anchor-center).normalized();
    const Vec3d direction{sun.direction[0],sun.direction[1],sun.direction[2]};
    Vec3d x=direction-up*direction.dot(up); if(x.norm()<1e-8) x=up.unitOrthogonal(); else x.normalize();
    Mat3d basis; basis.row(0)=x; basis.row(1)=up; basis.row(2)=x.cross(up);
    const Mat3d camera=basis*view.description().camera_world.matrix().topLeftCorner<3,3>();
    const auto& projection_matrix=view.description().projection;
    auto pack=[](const auto& v,double w,double scale=1) { return std::array<float,4>{float(v[0]*scale),float(v[1]*scale),float(v[2]*scale),float(w)}; };
    AerialParameters p{};
    const double height=take(atmosphere_height_m(a,{anchor.x(),anchor.y(),anchor.z()}));
    p.geometry={float(a.ground_radius_m*.001),float(a.atmosphere_height_m*.001),float(height*.001),float(up.dot(direction))};
    p.rayleigh=pack(a.rayleigh_scattering_per_m,a.rayleigh_scale_height_m*.001,1000);
    p.mie=pack(a.mie_scattering_per_m,a.mie_scale_height_m*.001,1000);
    p.extinction=pack(a.mie_extinction_per_m,a.mie_anisotropy,1000);
    p.absorption=pack(a.absorption_extinction_per_m,a.absorption_center_height_m*.001,1000);
    p.ground=pack(a.ground_albedo,a.absorption_half_width_m*.001);
    const double sine=std::sin(sun.angular_radius_rad),half_sine=std::sin(sun.angular_radius_rad*.5);
    p.sun=pack(sun.irradiance,1/(std::numbers::pi*sine*sine));
    p.sun_direction=pack((basis*direction).eval(),4*half_sine*half_sine);
    p.forward=pack((camera*Vec3d{projection_matrix(0,2)/projection_matrix(0,0),projection_matrix(1,2)/projection_matrix(1,1),-1}).eval(),0);
    p.right=pack((camera.col(0)/projection_matrix(0,0)).eval(),0);
    p.up=pack((camera.col(1)/projection_matrix(1,1)).eval(),take(sky.multiple_scattering_enabled()) ? 1 : 0);
    p.frame={view.description().width,view.description().height,settings.reference ? 1u : 0u,settings.sun_disk ? 1u : 0u};
    p.depth={float(projection_matrix(2,2)),float(projection_matrix(2,3)*.001),float(projection.far_m*.001),0};
    auto uniform=take(state.factory.create_buffer({sizeof(p),vk::BufferUsageFlagBits::eUniformBuffer,BufferMemory::upload}));
    check(uniform.write(0,std::as_bytes(std::span{&p,1})));
    if(!state.aerial[0].pipeline) {
        std::array<detail::PipelineState::Program,2> programs;
        for(unsigned i=0;i<2;++i) {
            const auto side=std::to_string(aerial_side),depth=std::to_string(aerial_depth);
            const std::array defines{ShaderDefine{"DK_AIR_SIDE",side},ShaderDefine{"DK_AIR_DEPTH",depth},ShaderDefine{"DK_VOLUME","1"}};
            auto code=take(compile_shader({state.directory/"aerial.slang","computeMain",ShaderStage::compute,{},
                std::span<const ShaderDefine>{defines}.first(i==0 ? 3 : 2)},state.heap));
            auto shader=take(state.factory.create_shader(code)); const std::array shaders{&shader};
            programs[i].layout=take(state.factory.create_pipeline_layout(shaders));
            programs[i].pipeline=take(state.factory.create_compute_pipeline({&shader,&programs[i].layout}));
        }
        SamplerDesc desc; desc.address_u=desc.address_v=desc.address_w=vk::SamplerAddressMode::eClampToEdge;
        auto sampler=take(state.factory.create_sampler(desc));
        state.aerial=std::move(programs); state.aerial_sampler=std::move(sampler);
    }
    return uniform;
}
void prepare_sunlight(detail::PipelineState& state) {
    if(state.sunlight) return;
    auto compile=[&](const char* entry,ShaderStage stage,std::string_view define={}) {
        const std::array definitions{ShaderDefine{define,"1"}};
        auto code=take(compile_shader({state.directory/"sunlight.slang",entry,stage,{},define.empty() ?
            std::span<const ShaderDefine>{} : std::span<const ShaderDefine>{definitions}},state.heap));
        return take(state.factory.create_shader(code));
    };
    auto vs=compile("vertexMain",ShaderStage::vertex,"DK_VERTEX");
    auto fs=compile("fragmentMain",ShaderStage::fragment);
    auto ds=compile("depthMain",ShaderStage::fragment,"DK_DEPTH");
    const std::array shaders{&vs,&fs},depth_shaders{&vs,&ds};
    auto layout=take(state.factory.create_pipeline_layout(shaders));
    auto depth_layout=take(state.factory.create_pipeline_layout(depth_shaders));
    const std::array vb{vk::VertexInputBindingDescription{0,sizeof(GpuVertex),vk::VertexInputRate::eVertex}};
    const std::array va{vk::VertexInputAttributeDescription{0,0,vk::Format::eR32G32B32Sfloat,offsetof(GpuVertex,position)},
        vk::VertexInputAttributeDescription{1,0,vk::Format::eR32G32Sfloat,offsetof(GpuVertex,uv)},
        vk::VertexInputAttributeDescription{2,0,vk::Format::eR32G32B32Sfloat,offsetof(GpuVertex,normal)}};
    GraphicsPipelineDesc depth{&vs,&ds,&depth_layout,vb,va};
    depth.color_format=vk::Format::eUndefined; depth.depth_format=vk::Format::eD32Sfloat;
    depth.depth_test=depth.depth_write=true;
    auto depth_pipeline=take(state.factory.create_graphics_pipeline(depth));
    GraphicsPipelineDesc color{&vs,&fs,&layout,vb,va};
    color.color_format=vk::Format::eR32G32B32A32Sfloat; color.depth_format=vk::Format::eD32Sfloat;
    color.depth_test=true; color.depth_compare=vk::CompareOp::eEqual;
    auto pipeline=take(state.factory.create_graphics_pipeline(color));
    SamplerDesc desc; desc.address_u=desc.address_v=desc.address_w=vk::SamplerAddressMode::eClampToEdge;
    auto sampler=take(state.factory.create_sampler(desc));
    state.sunlight_depth_layout=std::move(depth_layout); state.sunlight_layout=std::move(layout);
    state.sunlight_depth=std::move(depth_pipeline); state.sunlight=std::move(pipeline); state.sky_sampler=std::move(sampler);
}
void resolve_sunlight(Inputs& inputs,const RenderView& view,const AtmosphereSky& sky,const RenderSettings& settings,detail::PipelineState& state) {
    const auto environment=take(sky.environment());
    if(environment!=view.scene().environment()) throw Error{ErrorCode::invalid_argument,"lighting snapshot does not match scene environment"};
    const auto& a=environment.atmosphere;
    const Vec3d center{a.planet_center_m[0],a.planet_center_m[1],a.planet_center_m[2]};
    const Vec3d anchor=take(sky.position_m()),up=(anchor-center).normalized();
    const Vec3d sun{environment.sun.direction[0],environment.sun.direction[1],environment.sun.direction[2]};
    const double height=take(atmosphere_height_m(a,{anchor.x(),anchor.y(),anchor.z()}));
    Vec3d x=sun-up*sun.dot(up); if(x.norm()<1e-8) x=up.unitOrthogonal(); else x.normalize();
    Mat3d basis; basis.row(0)=x; basis.row(1)=up; basis.row(2)=x.cross(up);
    const Vec3d local_sun=basis*sun;
    inputs.transmittance=take(sky.image(AtmosphereLut::transmittance)); inputs.sky=take(sky.image(AtmosphereLut::sky_view));
    struct Parameters { std::array<float,4> normal[3],relative[3],geometry,sun,energy; };
    static_assert(sizeof(Parameters)==144);
    for(auto& draw:inputs.draws) {
        const auto& world=draw.world.matrix();
        const Mat3d normal=basis*take(detail::normal_matrix(draw.world));
        Mat4d relative=world; relative.topRightCorner<3,1>()-=anchor;
        relative.topRows<3>()=(basis*relative.topRows<3>()*.001).eval();
        if(!relative.allFinite() || (relative.array().abs()>double(std::numeric_limits<float>::max())).any())
            throw Error{ErrorCode::invalid_argument,"lighting relative transform cannot be represented"};
        Vec3d low=Vec3d::Constant(std::numeric_limits<double>::infinity()),high=-low;
        for(unsigned corner=0;corner<8;++corner) {
            Vec3d v; for(unsigned c=0;c<3;++c) v[c]=(corner&(1u<<c)) ? draw.primitive->bounds_max[c] : draw.primitive->bounds_min[c];
            const Vec3d p=take(draw.world.transform_point(v));
            if((p.array().abs()>1e9).any()) throw Error{ErrorCode::not_supported,"lighting receiver exceeds world position domain"};
            low=low.cwiseMin(p); high=high.cwiseMax(p);
            if((p-center).stableNorm()-a.ground_radius_m>2*a.atmosphere_height_m)
                throw Error{ErrorCode::not_supported,"lighting receiver exceeds atmosphere height domain"};
        }
        const Vec3d nearest=center.cwiseMax(low).cwiseMin(high);
        if((nearest-center).stableNorm()<a.ground_radius_m)
            throw Error{ErrorCode::not_supported,"lighting receiver bounds intersect the planet"};
        Parameters p{};
        for(unsigned r=0;r<3;++r) {
            for(unsigned c=0;c<3;++c) p.normal[r][c]=float(normal(r,c));
            for(unsigned c=0;c<4;++c) p.relative[r][c]=float(relative(r,c));
        }
        p.normal[0][3]=draw.primitive->has_normals ? 1.f : 0.f;
        p.geometry={float(a.ground_radius_m*.001),float(a.atmosphere_height_m*.001),float(height*.001),settings.sky_diffuse ? 1.f : 0.f};
        const Mat3d linear=world.topLeftCorner<3,3>()/world.topLeftCorner<3,3>().cwiseAbs().maxCoeff();
        p.sun={float(local_sun.x()),float(local_sun.y()),float(local_sun.z()),linear.determinant()<0 ? -1.f : 1.f};
        p.energy={float(environment.sun.irradiance[0]),float(environment.sun.irradiance[1]),float(environment.sun.irradiance[2]),draw.double_sided ? 1.f : 0.f};
        draw.lighting=take(state.factory.create_buffer({sizeof(p),vk::BufferUsageFlagBits::eUniformBuffer,BufferMemory::upload}));
        check(draw.lighting.write(0,std::as_bytes(std::span{&p,1})));
    }
    for(auto& draw:inputs.draws) { inputs.buffers.push_back(&draw.lighting); draw.lighting_index=inputs.buffers.size(); }
}
#endif
graph::Use use(graph::ResourceId id,vk::PipelineStageFlags2 stages,vk::AccessFlags2 access,
    vk::ImageLayout layout=vk::ImageLayout::eUndefined,bool overwrite=false,bool depth=false) {
    return {id,{{stages,access,layout},0,VK_WHOLE_SIZE,depth ? depth_range : color_range,overwrite}};
}
struct Context {
    detail::PipelineState& pipeline;
    const Inputs& inputs;
    const RenderSettings& settings;
    std::uint32_t width, height;
    std::size_t hdr, depth, ldr, white;
    std::size_t composed=0,linear_readback=0,parameters=0;
    std::array<std::size_t,3> atmosphere_luts{};
    std::array<std::size_t,2> volumes{};
};
Result<void> clear_pass(graph::PassContext& pass,void* pointer) {
    const auto& c = *static_cast<Context*>(pointer);
    const auto& rgb = c.settings.clear_rgb;
    check(pass.clear(c.hdr,vk::ClearColorValue{std::array<float,4>{rgb.x(),rgb.y(),rgb.z(),1}},color_range));
    check(pass.clear_depth(c.depth,1,depth_range));
    check(pass.clear(c.ldr,vk::ClearColorValue{std::array<float,4>{0,0,0,1}},color_range));
    check(pass.clear(c.white,vk::ClearColorValue{std::array<float,4>{1,1,1,1}},color_range));
    return {};
}
Result<void> geometry_pass(graph::PassContext& pass,void* pointer) {
    auto& c = *static_cast<Context*>(pointer);
    const bool depth_only = pass.pass_index() == 1;
    const bool lit=c.inputs.sky!=nullptr;
    auto dv = take(c.pipeline.factory.create_view(*take(pass.image(c.depth)),{vk::ImageViewType::e2D,depth_range}));
    ImageView cv,white;
    RenderingDesc desc{}; desc.depth.view = &dv; desc.depth.load = vk::AttachmentLoadOp::eLoad;
    if (!depth_only) {
        cv = take(c.pipeline.factory.create_view(*take(pass.image(c.hdr))));
        desc.color.view = &cv; desc.color.load = vk::AttachmentLoadOp::eLoad;
    }
    white = take(c.pipeline.factory.create_view(*take(pass.image(c.white))));
    auto encoder = take(pass.begin_rendering(desc));
    for (const auto& draw : c.inputs.draws) {
        check(encoder.bind_pipeline(lit ? (depth_only ? c.pipeline.sunlight_depth : c.pipeline.sunlight) :
            (depth_only ? (draw.masked ? c.pipeline.mask : c.pipeline.depth) : c.pipeline.opaque)));
        check(encoder.vertex_buffer(0,*take(pass.buffer(draw.vertices))));
        check(encoder.index_buffer(*take(pass.buffer(draw.indices)),vk::IndexType::eUint32));
        if (depth_only && !draw.masked && !lit) check(encoder.push_constants(vk::ShaderStageFlagBits::eVertex,0,std::as_bytes(std::span{draw.push})));
        else {
            const auto& view = draw.texture ? draw.texture->view : white;
            const auto& sampler = draw.texture ? draw.texture->sampler : c.pipeline.white_sampler;
            std::array<BindingWrite,6> writes{BindingWrite{0,0,ImageBinding{&view}},BindingWrite{1,0,SamplerBinding{&sampler}}};
            std::size_t count=2; ImageView transmittance,sky;
            if(lit) {
                writes[count++]={2,0,BufferBinding{take(pass.buffer(draw.lighting_index))}};
                if(!depth_only) {
                    transmittance=take(c.pipeline.factory.create_view(*c.inputs.transmittance));
                    sky=take(c.pipeline.factory.create_view(*c.inputs.sky));
                    writes[count++]={3,0,ImageBinding{&transmittance}};
                    writes[count++]={4,0,ImageBinding{&sky}};
                    writes[count++]={5,0,SamplerBinding{&c.pipeline.sky_sampler}};
                }
            }
            const auto& layout=lit ? (depth_only ? c.pipeline.sunlight_depth_layout : c.pipeline.sunlight_layout) :
                (depth_only ? c.pipeline.mask_layout : c.pipeline.opaque_layout);
            auto bindings = take(c.pipeline.factory.create_bindings(layout,0,std::span{writes}.first(count)));
            const std::array sets{&bindings}; check(encoder.bind_sets(sets));
            check(encoder.push_constants(vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment,0,std::as_bytes(std::span{draw.push})));
        }
        check(encoder.draw_indexed(draw.primitive->index_count));
    }
    return encoder.end();
}
Result<void> tone_pass(graph::PassContext& pass,void* pointer) {
    auto& c = *static_cast<Context*>(pointer);
    auto input = take(c.pipeline.factory.create_view(*take(pass.image(c.composed ? c.composed : c.hdr))));
    auto output = take(c.pipeline.factory.create_view(*take(pass.image(c.ldr))));
    const std::array writes{BindingWrite{0,0,ImageBinding{&input}}};
    auto bindings = take(c.pipeline.factory.create_bindings(c.pipeline.tone_layout,0,writes));
    RenderingDesc desc{}; desc.color.view = &output; desc.color.load = vk::AttachmentLoadOp::eLoad;
    auto encoder = take(pass.begin_rendering(desc)); check(encoder.bind_pipeline(c.pipeline.tone));
    const std::array sets{&bindings}; check(encoder.bind_sets(sets));
    const std::array<float,4> parameters{c.settings.exposure,0,0,0};
    check(encoder.push_constants(vk::ShaderStageFlagBits::eFragment,0,std::as_bytes(std::span{parameters})));
    check(encoder.draw(3)); return encoder.end();
}
Result<void> aerial_pass(graph::PassContext& pass,void* pointer) {
    auto& c=*static_cast<Context*>(pointer);
    const bool volume=pass.pass_index()==3;
    auto& program=c.pipeline.aerial[volume ? 0 : 1];
    std::array<ImageView,8> views;
    std::array<BindingWrite,10> writes{BindingWrite{0,0,BufferBinding{take(pass.buffer(c.parameters))}},
        BindingWrite{2,0,SamplerBinding{&c.pipeline.aerial_sampler}}};
    std::size_t count=2;
    for(unsigned i=0;i<(volume ? 2u : 3u);++i) {
        views[i]=take(c.pipeline.factory.create_view(*take(pass.image(c.atmosphere_luts[i]))));
        writes[count++]={3+i,0,ImageBinding{&views[i]}};
    }
    for(unsigned i=0;i<2;++i) {
        views[3+i]=take(c.pipeline.factory.create_view(*take(pass.image(c.volumes[i])),{vk::ImageViewType::e3D,color_range}));
        writes[count++]={6+i,0,ImageBinding{&views[3+i],volume ? vk::ImageLayout::eGeneral : vk::ImageLayout::eShaderReadOnlyOptimal}};
    }
    if(!volume) {
        views[5]=take(c.pipeline.factory.create_view(*take(pass.image(c.composed))));
        views[6]=take(c.pipeline.factory.create_view(*take(pass.image(c.depth)),{vk::ImageViewType::e2D,depth_range}));
        views[7]=take(c.pipeline.factory.create_view(*take(pass.image(c.hdr))));
        writes[count++]={1,0,ImageBinding{&views[5],vk::ImageLayout::eGeneral}};
        writes[count++]={8,0,ImageBinding{&views[6],vk::ImageLayout::eShaderReadOnlyOptimal}};
        writes[count++]={9,0,ImageBinding{&views[7]}};
    }
    auto bindings=take(c.pipeline.factory.create_bindings(program.layout,0,std::span{writes}.first(count)));
    auto encoder=take(pass.compute()); check(encoder.bind_pipeline(program.pipeline));
    const std::array sets{&bindings}; check(encoder.bind_sets(sets));
    return encoder.dispatch({volume ? (aerial_side+7)/8 : (c.width+7)/8,volume ? (aerial_side+7)/8 : (c.height+7)/8,volume ? aerial_depth : 1u});
}
template<auto Record> Result<void> record(graph::PassContext& pass,void* context) {
    try { return Record(pass,context); }
    catch (Error& error) { return std::unexpected(std::move(error)); }
}
Result<void> copy_pass(graph::PassContext& pass,void* pointer) {
    const auto& c = *static_cast<Context*>(pointer);
    check(pass.copy_to_buffer(c.ldr,0,{0,0,0,0,c.width,c.height}));
    if(c.linear_readback) check(pass.copy_to_buffer(c.composed ? c.composed : c.hdr,c.linear_readback,{0,0,0,0,c.width,c.height}));
    return {};
}
}
Result<void> validate_render_settings(const RenderView& view,const RenderSettings& settings) {
    const auto& desc = view.description();
    if (!view.scene() || !desc.width || !desc.height ||
        std::uint64_t{desc.width}*desc.height > std::numeric_limits<std::size_t>::max()/4 ||
        (settings.linear_readback && std::uint64_t{desc.width}*desc.height>65536) ||
        !std::isfinite(settings.exposure) || settings.exposure < 0 || !settings.clear_rgb.allFinite() || (settings.clear_rgb.array()<0).any())
        return std::unexpected(Error{ErrorCode::invalid_argument,"render requires a scene, addressable extent and finite nonnegative color/exposure"});
    return {};
}
Result<ScenePipeline> ScenePipeline::create(memory::ResourceHandle heap,SubmissionQueue& queue,const std::filesystem::path& directory) {
    if (!heap || heap.state()!=memory::ResourceState::open || queue.stats().closed || queue.stats().device_lost)
        return std::unexpected(Error{ErrorCode::invalid_state,"scene pipeline requires open memory and queue"});
    try {
        auto state = memory::make_shared_in<detail::PipelineState>(heap); state->heap = heap; state->factory = queue.resources(); state->directory=directory;
        auto compile = [&](const char* file,const char* entry,ShaderStage stage,std::string_view define={}) {
            const std::array definitions{ShaderDefine{define,"1"}};
            auto shader = take(compile_shader({directory/file,entry,stage,{},define.empty() ? std::span<const ShaderDefine>{} : std::span<const ShaderDefine>{definitions}},heap));
            return take(state->factory.create_shader(shader));
        };
        auto opaque_vs = compile("scene.slang","vertexMain",ShaderStage::vertex,"DK_VERTEX");
        auto opaque_fs = compile("scene.slang","fragmentMain",ShaderStage::fragment);
        auto mask_fs = compile("scene.slang","depthMain",ShaderStage::fragment,"DK_DEPTH");
        auto tone_vs = compile("tone.slang","vertexMain",ShaderStage::vertex,"DK_VERTEX");
        auto tone_fs = compile("tone.slang","fragmentMain",ShaderStage::fragment);
        const std::array ds{&opaque_vs};
        const std::array os{&opaque_vs,&opaque_fs};
        const std::array ms{&opaque_vs,&mask_fs};
        const std::array ts{&tone_vs,&tone_fs};
        state->depth_layout = take(state->factory.create_pipeline_layout(ds));
        state->mask_layout = take(state->factory.create_pipeline_layout(ms));
        state->opaque_layout = take(state->factory.create_pipeline_layout(os));
        state->tone_layout = take(state->factory.create_pipeline_layout(ts));
        const std::array vb{vk::VertexInputBindingDescription{0,sizeof(GpuVertex),vk::VertexInputRate::eVertex}};
        const std::array va{vk::VertexInputAttributeDescription{0,0,vk::Format::eR32G32B32Sfloat,offsetof(GpuVertex,position)},
            vk::VertexInputAttributeDescription{1,0,vk::Format::eR32G32Sfloat,offsetof(GpuVertex,uv)}};
        GraphicsPipelineDesc depth{}; depth.vertex = &opaque_vs; depth.layout = &state->depth_layout;
        depth.vertex_bindings = vb; depth.vertex_attributes = va;
        depth.color_format = vk::Format::eUndefined; depth.depth_format = vk::Format::eD32Sfloat; depth.depth_test = depth.depth_write = true;
        state->depth = take(state->factory.create_graphics_pipeline(depth));
        depth.fragment = &mask_fs; depth.layout = &state->mask_layout;
        state->mask = take(state->factory.create_graphics_pipeline(depth));
        GraphicsPipelineDesc opaque{&opaque_vs,&opaque_fs,&state->opaque_layout,vb,va};
        opaque.color_format = vk::Format::eR32G32B32A32Sfloat; opaque.depth_format = vk::Format::eD32Sfloat;
        opaque.depth_test = true; opaque.depth_compare = vk::CompareOp::eEqual;
        state->opaque = take(state->factory.create_graphics_pipeline(opaque));
        state->tone = take(state->factory.create_graphics_pipeline({&tone_vs,&tone_fs,&state->tone_layout}));
        state->white_sampler = take(state->factory.create_sampler());
        return ScenePipeline{std::move(state)};
    } catch (Error& error) { return std::unexpected(std::move(error)); }
    catch (const std::bad_alloc&) { return std::unexpected(allocation_error()); }
}
Result<RenderFrame> ScenePipeline::render(SubmissionQueue& queue,const RenderView& view,const GpuAssets& assets,const RenderSettings& settings) {
    return render_impl(queue,view,assets,settings,nullptr);
}
Result<RenderFrame> ScenePipeline::render_sunlight(SubmissionQueue& queue,const RenderView& view,const GpuAssets& assets,const AtmosphereSky& sky,const RenderSettings& settings) {
    return render_impl(queue,view,assets,settings,view.scene().environment().enabled ? &sky : nullptr);
}
Result<RenderFrame> ScenePipeline::render_environment(SubmissionQueue& queue,const RenderView& view,const GpuAssets& assets,const RenderSettings& settings) {
    if(view.scene().environment().clouds.enabled)
        return std::unexpected(Error{ErrorCode::not_supported,"cloud field resources are available; cloud rendering is not implemented"});
    if(!view.scene().environment().enabled) return render(queue,view,assets,settings);
#ifdef DK_RENDER_SUNLIGHT
    if(!state_) return std::unexpected(Error{ErrorCode::invalid_state,"empty scene pipeline"});
    std::optional<AtmosphereRenderer> candidate;
    if(!state_->atmosphere) {
        auto renderer=AtmosphereRenderer::create(state_->heap,queue,state_->directory/"../../engine/render/atmosphere/shaders");
        if(!renderer) return std::unexpected(renderer.error()); candidate=std::move(*renderer);
    }
    auto& renderer=state_->atmosphere ? *state_->atmosphere : *candidate;
    auto sky=renderer.prepare(queue,view.scene().environment(),view.description().camera_world.matrix().topRightCorner<3,1>());
    if(!sky) return std::unexpected(sky.error());
    auto frame=render_atmosphere(queue,view,assets,*sky,settings);
    if(frame && candidate) state_->atmosphere=std::move(candidate);
    return frame;
#else
    return std::unexpected(Error{ErrorCode::not_supported,"environment rendering requires DK_BUILD_RENDER_ATMOSPHERE"});
#endif
}
Result<std::string> shader_fingerprint(const std::filesystem::path& directory,bool environment) {
    const std::array files{"scene.slang","tone.slang","sunlight.slang","aerial.slang",
        "../../engine/render/atmosphere/shaders/atmosphere.slang","../../engine/render/atmosphere/shaders/AtmosphereTransport.slang"};
    std::uint64_t hash=14695981039346656037ULL;
    auto append=[&](unsigned char c) { hash^=c; hash*=1099511628211ULL; };
    for(unsigned i=0;i<(environment ? files.size() : 2);++i) {
        for(const unsigned char c:std::string_view(files[i])) append(c); append(0);
        std::ifstream stream(directory/files[i],std::ios::binary);
        if(!stream) return std::unexpected(Error{ErrorCode::io_error,"cannot read render shader fingerprint input"});
        char c; while(stream.get(c)) append(static_cast<unsigned char>(c));
        if(!stream.eof()) return std::unexpected(Error{ErrorCode::io_error,"cannot finish render shader fingerprint"});
        append(0);
    }
    // CPU-side sampling/mapping version, including the fixed 64 x 64 x 128 AP grid.
    for(const unsigned char c:std::string_view{"render-environment-v1:64x64x128:128"}) append(c);
    std::ostringstream out; out<<"fnv1a64:"<<std::hex<<std::setfill('0')<<std::setw(16)<<hash; return out.str();
}
Result<RenderFrame> ScenePipeline::render_atmosphere(SubmissionQueue& queue,const RenderView& view,const GpuAssets& assets,const AtmosphereSky& sky,const RenderSettings& settings,const AtmosphereSettings& atmosphere) {
    const bool enabled=view.scene().environment().enabled;
    return render_impl(queue,view,assets,settings,enabled ? &sky : nullptr,enabled ? &atmosphere : nullptr);
}
Result<RenderFrame> ScenePipeline::render_impl(SubmissionQueue& queue,const RenderView& view,const GpuAssets& assets,const RenderSettings& settings,const AtmosphereSky* sky,const AtmosphereSettings* atmosphere) {
    DK_PROFILE_ZONE("render.scene");
    if (!state_ || state_->heap.state()!=memory::ResourceState::open || queue.stats().closed || queue.stats().device_lost)
        return std::unexpected(Error{ErrorCode::invalid_state,"scene pipeline, memory or queue is unavailable"});
    if (auto valid=validate_render_settings(view,settings); !valid) return std::unexpected(valid.error());
    const auto width=view.description().width, height=view.description().height;
    const auto& limits=queue.device().adapter().properties.limits;
    if (width>limits.maxFramebufferWidth || height>limits.maxFramebufferHeight || width>limits.maxViewportDimensions[0] || height>limits.maxViewportDimensions[1])
        return std::unexpected(Error{ErrorCode::not_supported,"render extent exceeds device limits"});
    try {
        const auto heap=state_->heap;
        Inputs inputs{heap}; resolve(inputs,view,assets);
        Buffer aerial_uniform;
        if(sky) {
#ifdef DK_RENDER_SUNLIGHT
            if(atmosphere) aerial_uniform=prepare_aerial(*state_,view,*sky,*atmosphere);
            if(!atmosphere || atmosphere->surface_lighting) {
                resolve_sunlight(inputs,view,*sky,settings,*state_);
                prepare_sunlight(*state_);
            }
#else
            return std::unexpected(Error{ErrorCode::not_supported,"environment rendering requires DK_BUILD_RENDER_ATMOSPHERE"});
#endif
        }
        auto frame=memory::make_shared_in<detail::FrameState>(heap);
        frame->info={view.scene().id(),view.scene().revision(),view.description().frame,width,height,inputs.draws.size()};
        auto graph=take(graph::Graph::create(heap));
        const auto readback=take(graph.declare_buffer("RGBA8 readback",{vk::DeviceSize{width}*height*4,vk::BufferUsageFlagBits::eTransferDst,BufferMemory::readback}));
        Vector<graph::ExternalBinding> external(0,memory::Allocator<graph::ExternalBinding>{heap});
        Vector<graph::Use> geometry(0,memory::Allocator<graph::Use>{heap});
        for (std::size_t i=0;i<inputs.buffers.size();++i) {
            const auto* buffer=inputs.buffers[i];
            const auto id=take(graph.declare_buffer("mesh-buffer-"+std::to_string(i),buffer->description(),graph::Lifetime::external,true));
            const bool vertex=bool(buffer->description().usage & vk::BufferUsageFlagBits::eVertexBuffer);
            const bool uniform=bool(buffer->description().usage & vk::BufferUsageFlagBits::eUniformBuffer);
            geometry.push_back(use(id,uniform ? (vk::PipelineStageFlagBits2::eVertexShader | vk::PipelineStageFlagBits2::eFragmentShader) :
                (vertex ? vk::PipelineStageFlagBits2::eVertexAttributeInput : vk::PipelineStageFlagBits2::eIndexInput),
                uniform ? vk::AccessFlagBits2::eUniformRead : (vertex ? vk::AccessFlagBits2::eVertexAttributeRead : vk::AccessFlagBits2::eIndexRead)));
            external.push_back({i+1,buffer,nullptr,{}});
        }
        graph::ResourceId constants{},linear_readback{};
        std::size_t parameters_index=0;
        if(atmosphere) {
            parameters_index=graph.counts().buffers;
            constants=take(graph.declare_buffer("aerial parameters",aerial_uniform.description(),graph::Lifetime::external,true));
            external.push_back({parameters_index,&aerial_uniform});
        }
        if(settings.linear_readback) {
            frame->linear_readback=graph.counts().buffers;
            linear_readback=take(graph.declare_buffer("linear RGBA32F readback",{vk::DeviceSize{width}*height*16,vk::BufferUsageFlagBits::eTransferDst,BufferMemory::readback}));
        }
        const auto image_base=graph.counts().buffers;
        const auto hdr=take(graph.declare_image("linear HDR",{width,height,vk::Format::eR32G32B32A32Sfloat,
            vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eTransferSrc}));
        const auto depth=take(graph.declare_image("scene depth",{width,height,vk::Format::eD32Sfloat,
            vk::ImageUsageFlagBits::eDepthStencilAttachment | vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled}));
        const auto ldr=take(graph.declare_image("sRGB RGBA8",{width,height,vk::Format::eR8G8B8A8Unorm,
            vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled}));
        const auto white=take(graph.declare_image("default white",{1,1}));
        Context context{*state_,inputs,settings,width,height,image_base,image_base+1,image_base+2,image_base+3};
        frame->color=context.ldr; context.linear_readback=frame->linear_readback; context.parameters=parameters_index;
        const std::array clear_uses{use(hdr,vk::PipelineStageFlagBits2::eClear,vk::AccessFlagBits2::eTransferWrite,vk::ImageLayout::eTransferDstOptimal,true),
            use(depth,vk::PipelineStageFlagBits2::eClear,vk::AccessFlagBits2::eTransferWrite,vk::ImageLayout::eTransferDstOptimal,true,true),
            use(ldr,vk::PipelineStageFlagBits2::eClear,vk::AccessFlagBits2::eTransferWrite,vk::ImageLayout::eTransferDstOptimal,true),
            use(white,vk::PipelineStageFlagBits2::eClear,vk::AccessFlagBits2::eTransferWrite,vk::ImageLayout::eTransferDstOptimal,true)};
        static_cast<void>(take(graph.add_pass({"clear targets",clear_uses})));
        geometry.push_back(use(depth,depth_stages,depth_access,vk::ImageLayout::eDepthStencilAttachmentOptimal,false,true));
        geometry.push_back(use(white,vk::PipelineStageFlagBits2::eFragmentShader,vk::AccessFlagBits2::eShaderSampledRead,vk::ImageLayout::eShaderReadOnlyOptimal));
        for (std::size_t i=0;i<inputs.textures.size();++i) {
            const auto* texture=inputs.textures[i];
            const auto id=take(graph.declare_image("base-texture-"+std::to_string(i),texture->description(),graph::Lifetime::external,true));
            external.push_back({image_base+4+i,nullptr,texture,{}});
            geometry.push_back(use(id,vk::PipelineStageFlagBits2::eFragmentShader,vk::AccessFlagBits2::eShaderSampledRead,vk::ImageLayout::eShaderReadOnlyOptimal));
        }
        static_cast<void>(take(graph.add_pass({"depth prepass",geometry})));
        std::array<graph::ResourceId,3> lut_ids{};
#ifdef DK_RENDER_SUNLIGHT
        if(sky) {
            const std::array luts{AtmosphereLut::transmittance,AtmosphereLut::multiple_scattering,AtmosphereLut::sky_view};
            for(unsigned i=0;i<3;++i) {
                if(!atmosphere && i==1) continue;
                const auto* image=take(sky->image(luts[i]));
                context.atmosphere_luts[i]=graph.counts().buffers+graph.counts().images;
                lut_ids[i]=take(graph.declare_image("atmosphere LUT "+std::to_string(i),image->description(),graph::Lifetime::external,true));
                external.push_back({context.atmosphere_luts[i],nullptr,image,{}});
                if(inputs.sky && i!=1) geometry.push_back(use(lut_ids[i],vk::PipelineStageFlagBits2::eFragmentShader,vk::AccessFlagBits2::eShaderSampledRead,vk::ImageLayout::eShaderReadOnlyOptimal));
            }
        }
#endif
        geometry.push_back(use(hdr,vk::PipelineStageFlagBits2::eColorAttachmentOutput,color_access,vk::ImageLayout::eColorAttachmentOptimal));
        static_cast<void>(take(graph.add_pass({"opaque",geometry})));
        auto final_hdr=hdr;
        if(atmosphere) {
            constexpr auto compute=vk::PipelineStageFlagBits2::eComputeShader;
            constexpr auto sampled=vk::AccessFlagBits2::eShaderSampledRead;
            std::array<graph::ResourceId,2> volumes;
            ImageDesc volume{aerial_side,aerial_side,vk::Format::eR32G32B32A32Sfloat,vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled};
            volume.type=vk::ImageType::e3D; volume.depth=aerial_depth; volume.require_linear_filter=true;
            for(unsigned i=0;i<2;++i) {
                context.volumes[i]=graph.counts().buffers+graph.counts().images;
                volumes[i]=take(graph.declare_image(i==0 ? "air radiance 3D" : "air transmittance 3D",volume));
            }
            const std::array volume_uses{use(constants,compute,vk::AccessFlagBits2::eUniformRead),
                use(lut_ids[0],compute,sampled,vk::ImageLayout::eShaderReadOnlyOptimal),
                use(lut_ids[1],compute,sampled,vk::ImageLayout::eShaderReadOnlyOptimal),
                use(volumes[0],compute,vk::AccessFlagBits2::eShaderStorageWrite,vk::ImageLayout::eGeneral,true),
                use(volumes[1],compute,vk::AccessFlagBits2::eShaderStorageWrite,vk::ImageLayout::eGeneral,true)};
            static_cast<void>(take(graph.add_pass({"aerial perspective volume",volume_uses})));
            context.composed=graph.counts().buffers+graph.counts().images;
            final_hdr=take(graph.declare_image("composed linear HDR",{width,height,vk::Format::eR32G32B32A32Sfloat,
                vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferSrc}));
            const std::array composite_uses{use(constants,compute,vk::AccessFlagBits2::eUniformRead),
                use(lut_ids[0],compute,sampled,vk::ImageLayout::eShaderReadOnlyOptimal),use(lut_ids[1],compute,sampled,vk::ImageLayout::eShaderReadOnlyOptimal),
                use(lut_ids[2],compute,sampled,vk::ImageLayout::eShaderReadOnlyOptimal),
                use(volumes[0],compute,sampled,vk::ImageLayout::eShaderReadOnlyOptimal),use(volumes[1],compute,sampled,vk::ImageLayout::eShaderReadOnlyOptimal),
                use(hdr,compute,sampled,vk::ImageLayout::eShaderReadOnlyOptimal),
                use(depth,compute,sampled,vk::ImageLayout::eShaderReadOnlyOptimal,false,true),
                use(final_hdr,compute,vk::AccessFlagBits2::eShaderStorageWrite,vk::ImageLayout::eGeneral,true)};
            static_cast<void>(take(graph.add_pass({"sky and geometry composition",composite_uses})));
        }
        const std::array tone_uses{use(final_hdr,vk::PipelineStageFlagBits2::eFragmentShader,vk::AccessFlagBits2::eShaderSampledRead,vk::ImageLayout::eShaderReadOnlyOptimal),
            use(ldr,vk::PipelineStageFlagBits2::eColorAttachmentOutput,color_access,vk::ImageLayout::eColorAttachmentOptimal)};
        static_cast<void>(take(graph.add_pass({"tone mapping",tone_uses})));
        Vector<graph::Use> read_uses(0,memory::Allocator<graph::Use>{heap});
        read_uses={use(ldr,vk::PipelineStageFlagBits2::eCopy,vk::AccessFlagBits2::eTransferRead,vk::ImageLayout::eTransferSrcOptimal),
            use(readback,vk::PipelineStageFlagBits2::eCopy,vk::AccessFlagBits2::eTransferWrite,vk::ImageLayout::eUndefined,true)};
        if(settings.linear_readback) {
            read_uses.push_back(use(final_hdr,vk::PipelineStageFlagBits2::eCopy,vk::AccessFlagBits2::eTransferRead,vk::ImageLayout::eTransferSrcOptimal));
            read_uses.push_back(use(linear_readback,vk::PipelineStageFlagBits2::eCopy,vk::AccessFlagBits2::eTransferWrite,vk::ImageLayout::eUndefined,true));
            check(graph.mark_output(linear_readback));
        }
        static_cast<void>(take(graph.add_pass({"readback",read_uses})));
        check(graph.mark_output(readback)); check(graph.mark_output(ldr));
        auto plan=take(graph.compile());
        if (settings.capture_plan) frame->report=take(graph::format_plan(plan));
        Vector<graph::PassCallback> callbacks(0,memory::Allocator<graph::PassCallback>{heap});
        callbacks={{0,record<clear_pass>,&context},{1,record<geometry_pass>,&context},{2,record<geometry_pass>,&context}};
        if(atmosphere) { callbacks.push_back({3,record<aerial_pass>,&context}); callbacks.push_back({4,record<aerial_pass>,&context}); }
        callbacks.push_back({callbacks.size(),record<tone_pass>,&context}); callbacks.push_back({callbacks.size(),record<copy_pass>,&context});
        Vector<graph::FinalAccess> finals(0,memory::Allocator<graph::FinalAccess>{heap});
        finals={{0,{{vk::PipelineStageFlagBits2::eHost,vk::AccessFlagBits2::eHostRead}}},
            {context.ldr,{{vk::PipelineStageFlagBits2::eFragmentShader,vk::AccessFlagBits2::eShaderSampledRead,vk::ImageLayout::eShaderReadOnlyOptimal}}}};
        if(settings.linear_readback) finals.push_back({context.linear_readback,{{vk::PipelineStageFlagBits2::eHost,vk::AccessFlagBits2::eHostRead}}});
        frame->execution=take(graph::execute(plan,queue,{external,callbacks,finals}));
        return RenderFrame{std::move(frame)};
    } catch (Error& error) { return std::unexpected(std::move(error)); }
    catch (const std::bad_alloc&) { return std::unexpected(allocation_error()); }
}
FrameInfo RenderFrame::info() const noexcept { return state_ ? state_->info : FrameInfo{}; }
Submission RenderFrame::submission() const noexcept { return state_ ? state_->execution.submission() : Submission{}; }
Result<bool> RenderFrame::wait(SubmissionQueue& queue,std::uint64_t timeout) const {
    if (!state_) return std::unexpected(Error{ErrorCode::invalid_state,"empty render frame"});
    return queue.wait(submission(),timeout);
}
Result<void> RenderFrame::read_rgba8(std::span<std::byte> destination) const {
    if (!state_) return std::unexpected(Error{ErrorCode::invalid_state,"empty render frame"});
    if (destination.size()!=std::uint64_t{state_->info.width}*state_->info.height*4)
        return std::unexpected(Error{ErrorCode::invalid_argument,"frame readback byte count differs"});
    auto buffer=state_->execution.buffer(0); if (!buffer) return std::unexpected(buffer.error());
    return (*buffer)->read(0,destination);
}
Result<void> RenderFrame::read_linear_rgba32f(std::span<std::array<float,4>> destination) const {
    if(!state_ || !state_->linear_readback) return std::unexpected(Error{ErrorCode::invalid_state,"linear readback was not enabled"});
    if(destination.size()!=std::uint64_t{state_->info.width}*state_->info.height)
        return std::unexpected(Error{ErrorCode::invalid_argument,"linear readback pixel count differs"});
    auto buffer=state_->execution.buffer(state_->linear_readback); if(!buffer) return std::unexpected(buffer.error());
    return (*buffer)->read(0,std::as_writable_bytes(destination));
}
Result<const Image*> RenderFrame::color() const {
    if (!state_) return std::unexpected(Error{ErrorCode::invalid_state,"empty render frame"});
    return state_->execution.image(state_->color);
}
std::string_view RenderFrame::plan_text() const noexcept { return state_ ? state_->report.text() : std::string_view{}; }
} // namespace dk::render

#include "PipelinePolicy.hpp"
#include <dk/graphics/GraphExecution.hpp>
#include <dk/graphics/GraphDiagnostics.hpp>
#include <dk/graphics/ShaderCompiler.hpp>
#include <dk/memory/SmartPtr.hpp>
#include <dk/profiling/Profiler.hpp>
#include <algorithm>
#include <bit>
#ifdef DK_RENDER_SUNLIGHT
#include <dk/render/Atmosphere.hpp>
#endif
#ifdef DK_RENDER_CLOUDS
#include <dk/render/CloudReference.hpp>
#include <dk/render/CloudTextures.hpp>
#include <dk/render/CloudShadowRenderer.hpp>
#endif
#include <new>
#include <numbers>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace dk::render {
using namespace graphics;
namespace detail {
#ifdef DK_RENDER_CLOUDS
struct CloudEnvironment { CloudFieldCache fields; std::unique_ptr<CloudTextureCache> textures; std::optional<CloudShadowRenderer> shadows; };
struct CloudHistoryGeneration {
    CloudOptimizedSettings settings;
    ViewDescription view;
    SceneId scene;
    std::uint64_t revision=0,sequence=0;
    EnvironmentDescription environment;
    std::array<Image,3> images;
    Submission submission;
};
struct CloudHistorySlot {
    std::uint64_t id=0,sequence=0;
    std::shared_ptr<CloudHistoryGeneration> completed,pending;
    bool failed=false;
};
#endif
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
    Buffer identity;
    Program cloud_program;
    std::array<Program,3> cloud_optimized;
    std::filesystem::path directory;
#ifdef DK_RENDER_SUNLIGHT
    std::optional<AtmosphereRenderer> atmosphere;
#endif
#ifdef DK_RENDER_CLOUDS
    std::unique_ptr<CloudEnvironment> clouds;
    std::array<CloudHistorySlot,4> cloud_history;
#endif
};
struct FrameState {
    FrameInfo info;
    std::size_t color = 0, linear_readback = 0;
    std::array<std::size_t,2> cloud_readback{};
    std::size_t cloud_statistics=0;
    CloudFrameInfo cloud_info;
    std::uint64_t shadow_generation=0;
#ifdef DK_RENDER_CLOUDS
    CloudShadowMap shadow;
#endif
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
bool active_clouds(const EnvironmentDescription& e) {
    return e.clouds.enabled && e.clouds.coverage>0 && e.clouds.density>0 && e.clouds.extinction_per_m>0;
}
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
    const Image* shadow=nullptr;
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
Buffer prepare_aerial(detail::PipelineState& state,const RenderView& view,const AtmosphereSky& sky,const AtmosphereSettings& settings,bool clouds=false) {
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
    if(!clouds && !state.aerial[0].pipeline) {
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
#ifdef DK_RENDER_CLOUDS
Buffer prepare_clouds(detail::PipelineState& state,const RenderView& view,const CloudTextures& textures,
    const CloudCompositionSettings& settings,const RenderSettings& render,const CloudOptimizedSettings* optimized=nullptr) {
    const auto& e=view.scene().environment(); const auto& c=e.clouds; const auto& a=e.atmosphere; const auto& s=settings.reference;
    if(optimized) check(validate_cloud_optimization(e,*optimized,view.description().width,view.description().height));
    else check(validate_cloud_composition(e,settings,std::size_t(view.description().width)*view.description().height));
    if(textures.key()!=cloud_field_key(c) || !textures.bytes()) throw Error{ErrorCode::invalid_argument,"Cloud textures do not match scene"};
    using F=std::array<float,4>;
    struct Parameters { F geometry,cloud,scales,lighting,sun,irradiance,sky,rayleigh,mie,absorption,air;
        std::array<std::uint32_t,4> counts,flags; F origin_geometry,origin_direction,weather,base,detail,segments,forward,right,up,clear; };
    static_assert(sizeof(Parameters)==368);
    auto pack=[](const auto& v,double w=0) { return F{float(v[0]),float(v[1]),float(v[2]),float(w)}; };
    const Vec3d anchor=view.description().camera_world.matrix().topRightCorner<3,1>();
    Vec3d origin=anchor-Vec3d{a.planet_center_m.data()};
    const double height=take(atmosphere_height_m(a,{anchor.x(),anchor.y(),anchor.z()}));
    if(height<0 || (anchor.array().abs()>1e9).any()) throw Error{ErrorCode::invalid_argument,"Cloud camera below ground or outside scene bounds"};
    Parameters p{}; p.geometry={float(a.ground_radius_m),float(a.atmosphere_height_m),float(c.bottom_height_m),float(c.top_height_m)};
    p.cloud={float(c.coverage),float(c.density),float(c.extinction_per_m),float(s.homogeneous_density)};
    p.scales={float(c.weather_scale_m),float(c.shape_scale_m),float(c.detail_scale_m),float(c.bottom_fade_fraction)};
    p.lighting={float(s.phase_g),float(s.single_scattering_albedo),float(s.multiple_scattering),float(c.top_fade_fraction)};
    p.sun=pack(e.sun.direction); p.irradiance=pack(e.sun.irradiance); p.sky=pack(s.sky_radiance);
    p.rayleigh=pack(a.rayleigh_scattering_per_m,a.rayleigh_scale_height_m); p.mie=pack(a.mie_extinction_per_m,a.mie_scale_height_m);
    p.absorption=pack(a.absorption_extinction_per_m,a.absorption_center_height_m);
    p.air={float(a.absorption_half_width_m),float(c.weather_resolution),float(c.base_resolution),0};
    p.counts={s.steps,s.sun_steps,s.atmosphere_steps,0}; p.flags={1,e.enabled?1u:0u,s.self_shadow?1u:0u,s.homogeneous?1u:0u};
    p.origin_geometry={float(height),0,float(origin.normalized().dot(Vec3d{e.sun.direction.data()})),0};
    origin.x()-=c.wind_velocity_mps[0]*s.time_seconds; origin.z()-=c.wind_velocity_mps[1]*s.time_seconds;
    auto coordinate=[](double value,double scale) { const double x=value/scale; return float(x-std::floor(x)); };
    p.weather={coordinate(origin.x(),c.weather_scale_m),coordinate(origin.z(),c.weather_scale_m),0,0};
    for(unsigned i=0;i<3;++i) { p.base[i]=coordinate(origin[i],c.shape_scale_m); p.detail[i]=coordinate(origin[i],c.detail_scale_m); }
    const Mat3d camera=view.description().camera_world.matrix().topLeftCorner<3,3>(); const auto& projection=view.description().projection;
    p.forward=pack((camera*Vec3d{projection(0,2)/projection(0,0),projection(1,2)/projection(1,1),-1}).eval());
    p.right=pack((camera.col(0)/projection(0,0)).eval()); p.up=pack((camera.col(1)/projection(1,1)).eval()); p.clear=pack(render.clear_rgb);
    if(!optimized && !state.cloud_program.pipeline) {
        auto code=take(compile_shader({state.directory/"cloud-composition.slang","computeMain",ShaderStage::compute},state.heap));
        auto shader=take(state.factory.create_shader(code)); const std::array shaders{&shader}; detail::PipelineState::Program program;
        program.layout=take(state.factory.create_pipeline_layout(shaders)); program.pipeline=take(state.factory.create_compute_pipeline({&shader,&program.layout}));
        SamplerDesc desc; desc.address_u=desc.address_v=desc.address_w=vk::SamplerAddressMode::eClampToEdge;
        auto sampler=take(state.factory.create_sampler(desc)); state.cloud_program=std::move(program); state.aerial_sampler=std::move(sampler);
    }
    auto buffer=take(state.factory.create_buffer({sizeof(p),vk::BufferUsageFlagBits::eUniformBuffer,BufferMemory::upload}));
    check(buffer.write(0,std::as_bytes(std::span{&p,1}))); return buffer;
}
CloudHistoryReset history_reset(const detail::CloudHistorySlot& slot,const RenderView& view,const CloudOptimizedSettings& settings) {
    if(slot.failed) return CloudHistoryReset::failure;
    if(!settings.temporal) return CloudHistoryReset::disabled;
    if(!slot.completed) return CloudHistoryReset::new_view;
    const auto& old=*slot.completed; const auto& now=view.description();
    if(old.view.width!=now.width || old.view.height!=now.height) return CloudHistoryReset::resize;
    if(old.view.projection!=now.projection) return CloudHistoryReset::projection;
    if(old.scene!=view.scene().id()) return CloudHistoryReset::scene;
    if(old.environment!=view.scene().environment()) return CloudHistoryReset::content;
    if(old.revision!=view.scene().revision()) return CloudHistoryReset::scene;
    auto a=old.settings,b=settings; a.composition.reference.time_seconds=b.composition.reference.time_seconds=0;
    a.camera_revision=b.camera_revision=0; a.composition.diagnostics=b.composition.diagnostics=false;
    if(a!=b) return CloudHistoryReset::quality;
    const auto& previous=old.view.camera_world.matrix(); const auto& current=now.camera_world.matrix();
    const double cosine=( (previous.topLeftCorner<3,3>().transpose()*current.topLeftCorner<3,3>()).trace()-1)*.5;
    if(old.settings.camera_revision!=settings.camera_revision ||
        (previous.topRightCorner<3,1>()-current.topRightCorner<3,1>()).norm()>500 || cosine<std::cos(std::numbers::pi/12)) return CloudHistoryReset::camera;
    const double delta=settings.composition.reference.time_seconds-old.settings.composition.reference.time_seconds;
    if(delta<0 || delta>1) return CloudHistoryReset::time;
    return CloudHistoryReset::none;
}
Buffer prepare_reconstruction(detail::PipelineState& state,const RenderView& view,const CloudOptimizedSettings& settings,
    const detail::CloudHistoryGeneration* previous) {
    if(!state.cloud_optimized[0].pipeline) {
        std::array<detail::PipelineState::Program,3> candidate;
        for(unsigned i=0;i<3;++i) {
            const auto stage=std::to_string(i); const std::array defines{ShaderDefine{"DK_CLOUD_STAGE",stage}};
            auto code=take(compile_shader({state.directory/"cloud-reconstruction.slang","computeMain",ShaderStage::compute,{},defines},state.heap));
            auto shader=take(state.factory.create_shader(code)); const std::array shaders{&shader};
            candidate[i].layout=take(state.factory.create_pipeline_layout(shaders));
            candidate[i].pipeline=take(state.factory.create_compute_pipeline({&shader,&candidate[i].layout}));
        }
        SamplerDesc desc; desc.address_u=desc.address_v=desc.address_w=vk::SamplerAddressMode::eClampToEdge;
        auto sampler=take(state.factory.create_sampler(desc)); state.cloud_optimized=std::move(candidate); state.aerial_sampler=std::move(sampler);
    }
    struct Parameters { std::array<std::uint32_t,4> size; std::array<float,4> controls,previous_clip[4],camera_delta; };
    static_assert(sizeof(Parameters)==112);
    Parameters p{}; const auto& v=view.description();
    p.size={(v.width+1)/2,(v.height+1)/2,previous ? 1u : 0u,settings.empty_space_skipping ? 1u : 0u};
    p.controls={settings.termination_transmittance,settings.history_weight,0,0};
    if(previous) {
        const auto& old=previous->view; const auto& environment=view.scene().environment();
        const Vec3d delta=v.camera_world.matrix().topRightCorner<3,1>()-old.camera_world.matrix().topRightCorner<3,1>();
        const double dt=settings.composition.reference.time_seconds-previous->settings.composition.reference.time_seconds;
        const Vec3d wind{environment.clouds.wind_velocity_mps[0]*dt,0,environment.clouds.wind_velocity_mps[1]*dt};
        Mat4d relative=Mat4d::Identity(); relative.topLeftCorner<3,3>()=old.camera_world.matrix().topLeftCorner<3,3>().transpose();
        relative.topRightCorner<3,1>()=relative.topLeftCorner<3,3>()*(delta-wind); const Mat4d clip=old.projection*relative;
        for(unsigned row=0;row<4;++row) for(unsigned column=0;column<4;++column) p.previous_clip[row][column]=float(clip(row,column));
        for(unsigned i=0;i<3;++i) p.camera_delta[i]=float(delta[i]);
    }
    auto buffer=take(state.factory.create_buffer({sizeof(p),vk::BufferUsageFlagBits::eUniformBuffer,BufferMemory::upload}));
    check(buffer.write(0,std::as_bytes(std::span{&p,1}))); return buffer;
}
#endif
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
void resolve_sunlight(Inputs& inputs,const RenderView& view,const AtmosphereSky& sky,const RenderSettings& settings,detail::PipelineState& state,const CloudShadowMap* shadow) {
    static_cast<void>(shadow); // Optional Clouds target; no resource dependency when disabled.
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
    struct Parameters { std::array<float,4> normal[3],relative[3],geometry,sun,energy,shadow_u,shadow_v; };
    static_assert(sizeof(Parameters)==176);
#ifdef DK_RENDER_CLOUDS
    if(shadow) {
        if(take(shadow->environment())!=environment) throw Error{ErrorCode::invalid_argument,"Cloud shadow environment mismatch"};
        inputs.shadow=take(shadow->image());
    }
#endif
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
#ifdef DK_RENDER_CLOUDS
            if(shadow) take(cloud_shadow_coordinates(environment,take(shadow->settings()),p));
#endif
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
#ifdef DK_RENDER_CLOUDS
        if(shadow) {
            const auto projection=take(shadow->projection());
            const Vec3d u=basis*projection.u*(1000/projection.extent_m),v=basis*projection.v*(1000/projection.extent_m);
            for(unsigned i=0;i<3;++i) { p.shadow_u[i]=float(u[i]); p.shadow_v[i]=float(v[i]); }
            p.shadow_u[3]=float(.5+(anchor-projection.center_m).dot(projection.u)/projection.extent_m);
            p.shadow_v[3]=float(.5+(anchor-projection.center_m).dot(projection.v)/projection.extent_m);
        }
#endif
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
    std::array<std::size_t,2> volumes{},cloud_images{},cloud_readback{};
    const CloudTextures* clouds=nullptr;
    std::size_t cloud_parameters=0;
    bool optimized=false;
    std::size_t reconstruction_parameters=0,statistics=0,statistics_readback=0;
    std::array<std::size_t,3> raw_cloud{},filtered_cloud{},previous_cloud{};
};
Result<void> clear_pass(graph::PassContext& pass,void* pointer) {
    const auto& c = *static_cast<Context*>(pointer);
    const auto& rgb = c.settings.clear_rgb;
    check(pass.clear(c.hdr,vk::ClearColorValue{std::array<float,4>{rgb.x(),rgb.y(),rgb.z(),1}},color_range));
    check(pass.clear_depth(c.depth,1,depth_range));
    check(pass.clear(c.ldr,vk::ClearColorValue{std::array<float,4>{0,0,0,1}},color_range));
    check(pass.clear(c.white,vk::ClearColorValue{std::array<float,4>{1,1,1,1}},color_range));
    if(c.optimized) check(pass.fill(c.statistics));
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
            std::array<BindingWrite,7> writes{BindingWrite{0,0,ImageBinding{&view}},BindingWrite{1,0,SamplerBinding{&sampler}}};
            std::size_t count=2; ImageView transmittance,sky,shadow;
            if(lit) {
                writes[count++]={2,0,BufferBinding{take(pass.buffer(draw.lighting_index))}};
                if(!depth_only) {
                    transmittance=take(c.pipeline.factory.create_view(*c.inputs.transmittance));
                    sky=take(c.pipeline.factory.create_view(*c.inputs.sky));
                    writes[count++]={3,0,ImageBinding{&transmittance}};
                    writes[count++]={4,0,ImageBinding{&sky}};
                    writes[count++]={5,0,SamplerBinding{&c.pipeline.sky_sampler}};
                    if(c.inputs.shadow) shadow=take(c.pipeline.factory.create_view(*c.inputs.shadow));
                    writes[count++]={6,0,ImageBinding{c.inputs.shadow ? &shadow : &white}};
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
#ifdef DK_RENDER_CLOUDS
Result<void> cloud_pass(graph::PassContext& pass,void* pointer) {
    auto& c=*static_cast<Context*>(pointer); auto& program=c.pipeline.cloud_program;
    std::array<ImageView,7> views;
    views[0]=take(c.pipeline.factory.create_view(*take(pass.image(c.composed))));
    for(unsigned i=0;i<2;++i) {
        views[1+i]=take(c.pipeline.factory.create_view(*take(pass.image(c.atmosphere_luts[i]))));
        views[3+i]=take(c.pipeline.factory.create_view(*take(pass.image(c.cloud_images[i]))));
    }
    views[5]=take(c.pipeline.factory.create_view(*take(pass.image(c.depth)),{vk::ImageViewType::e2D,depth_range}));
    views[6]=take(c.pipeline.factory.create_view(*take(pass.image(c.hdr))));
    const std::array writes{BindingWrite{0,0,BufferBinding{take(pass.buffer(c.parameters))}},
        BindingWrite{1,0,ImageBinding{&views[0],vk::ImageLayout::eGeneral}},BindingWrite{2,0,SamplerBinding{&c.pipeline.aerial_sampler}},
        BindingWrite{3,0,ImageBinding{&views[1]}},BindingWrite{4,0,ImageBinding{&views[2]}},
        BindingWrite{6,0,ImageBinding{&views[3],vk::ImageLayout::eGeneral}},BindingWrite{7,0,ImageBinding{&views[4],vk::ImageLayout::eGeneral}},
        BindingWrite{8,0,ImageBinding{&views[5]}},BindingWrite{9,0,ImageBinding{&views[6]}},
        BindingWrite{10,0,BufferBinding{take(pass.buffer(c.cloud_parameters))}},
        BindingWrite{11,0,ImageBinding{&c.clouds->view(CloudField::weather)}},BindingWrite{12,0,ImageBinding{&c.clouds->view(CloudField::base)}},
        BindingWrite{13,0,ImageBinding{&c.clouds->view(CloudField::detail)}},BindingWrite{14,0,SamplerBinding{&c.clouds->sampler()}}};
    auto bindings=take(c.pipeline.factory.create_bindings(program.layout,0,writes));
    auto encoder=take(pass.compute()); check(encoder.bind_pipeline(program.pipeline)); const std::array sets{&bindings};
    check(encoder.bind_sets(sets)); return encoder.dispatch({(c.width+7)/8,(c.height+7)/8,1});
}
Result<void> reconstruction_pass(graph::PassContext& pass,void* pointer) {
    auto& c=*static_cast<Context*>(pointer); const unsigned stage=unsigned(pass.pass_index()-3);
    auto& program=c.pipeline.cloud_optimized[stage];
    std::array<ImageView,14> views; unsigned view_count=0;
    std::array<BindingWrite,23> writes; unsigned count=0;
    auto buffer=[&](unsigned binding,std::size_t index) { writes[count++]={binding,0,BufferBinding{take(pass.buffer(index))}}; };
    auto image=[&](unsigned binding,std::size_t index,bool output=false,bool depth=false) {
        views[view_count]=take(c.pipeline.factory.create_view(*take(pass.image(index)),{vk::ImageViewType::e2D,depth ? depth_range : color_range}));
        writes[count++]={binding,0,ImageBinding{&views[view_count++],output ? vk::ImageLayout::eGeneral : vk::ImageLayout::eShaderReadOnlyOptimal}};
    };
    buffer(0,c.parameters); buffer(10,c.cloud_parameters); buffer(15,c.statistics); buffer(16,c.reconstruction_parameters);
    const auto outputs=stage==0 ? c.raw_cloud : stage==1 ? c.filtered_cloud : std::array{c.composed,c.cloud_images[0],c.cloud_images[1]};
    image(1,outputs[0],true); image(6,outputs[1],true); image(7,outputs[2],true);
    if(stage!=1) {
        writes[count++]={2,0,SamplerBinding{&c.pipeline.aerial_sampler}};
        image(3,c.atmosphere_luts[0]); image(4,c.atmosphere_luts[1]); image(8,c.depth,false,true);
        for(unsigned i=0;i<3;++i) writes[count++]={11+i,0,ImageBinding{&c.clouds->view(static_cast<CloudField>(i))}};
        writes[count++]={14,0,SamplerBinding{&c.clouds->sampler()}};
    }
    if(stage>0) for(unsigned i=0;i<3;++i) image(17+i,stage==1 ? c.raw_cloud[i] : c.filtered_cloud[i]);
    if(stage==1) for(unsigned i=0;i<3;++i) image(20+i,c.previous_cloud[i]);
    if(stage==2) image(9,c.hdr);
    auto bindings=take(c.pipeline.factory.create_bindings(program.layout,0,std::span{writes}.first(count)));
    auto encoder=take(pass.compute()); check(encoder.bind_pipeline(program.pipeline)); const std::array sets{&bindings}; check(encoder.bind_sets(sets));
    const auto width=stage==2 ? c.width : (c.width+1)/2,height=stage==2 ? c.height : (c.height+1)/2;
    return encoder.dispatch({(width+7)/8,(height+7)/8,1});
}
#endif
template<auto Record> Result<void> record(graph::PassContext& pass,void* context) {
    try { return Record(pass,context); }
    catch (Error& error) { return std::unexpected(std::move(error)); }
}
Result<void> copy_pass(graph::PassContext& pass,void* pointer) {
    const auto& c = *static_cast<Context*>(pointer);
    check(pass.copy_to_buffer(c.ldr,0,{0,0,0,0,c.width,c.height}));
    if(c.linear_readback) check(pass.copy_to_buffer(c.composed ? c.composed : c.hdr,c.linear_readback,{0,0,0,0,c.width,c.height}));
    if(c.cloud_readback[0]) for(unsigned i=0;i<2;++i)
        check(pass.copy_to_buffer(c.cloud_images[i],c.cloud_readback[i],{0,0,0,0,c.width,c.height}));
    if(c.statistics_readback) check(pass.copy_buffer(c.statistics,c.statistics_readback,sizeof(CloudTraceStatistics)));
    return {};
}
}
const char* cloud_history_reset_name(CloudHistoryReset value) noexcept {
    constexpr const char* names[]{"none","new_view","disabled","resize","projection","camera","scene","content","quality","time","failure"};
    const auto i=static_cast<unsigned>(value); return i<std::size(names) ? names[i] : "unknown";
}
Result<void> validate_environment_render_settings(const EnvironmentRenderSettings& s) {
    if((s.mode!=CloudRenderMode::reference && s.mode!=CloudRenderMode::temporal) || !std::isfinite(s.time_seconds) || std::abs(s.time_seconds)>1e9 ||
        !s.view_id || !s.shadow_center_m.allFinite() || (s.shadow_center_m.array().abs()>1e9).any() || !std::isfinite(s.shadow_extent_m) ||
        s.shadow_extent_m<16 || s.shadow_extent_m>65536 || !std::has_single_bit(s.shadow_resolution) || s.shadow_resolution<16 || s.shadow_resolution>512)
        return std::unexpected(Error{ErrorCode::invalid_argument,"Invalid environment mode, time, view identity or shadow region"});
    return {};
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
        state->identity=take(state->factory.create_buffer({4}));
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
Result<RenderFrame> ScenePipeline::render_sunlight(SubmissionQueue& queue,const RenderView& view,const GpuAssets& assets,const AtmosphereSky& sky,const RenderSettings& settings,const CloudShadowMap* shadow) {
    return render_impl(queue,view,assets,settings,view.scene().environment().enabled ? &sky : nullptr,nullptr,nullptr,nullptr,nullptr,shadow);
}
Result<RenderFrame> ScenePipeline::render_environment(SubmissionQueue& queue,const RenderView& view,const GpuAssets& assets,const RenderSettings& settings,const EnvironmentRenderSettings& environment) {
    if(auto valid=validate_environment_render_settings(environment); !valid) return std::unexpected(valid.error());
    const bool cloudy=active_clouds(view.scene().environment());
    if(!cloudy && !view.scene().environment().enabled) {
        auto frame=render(queue,view,assets,settings);
#ifdef DK_RENDER_CLOUDS
        if(frame) for(auto& slot:state_->cloud_history) if(slot.id==environment.view_id) slot={};
#endif
        return frame;
    }
#ifdef DK_RENDER_SUNLIGHT
    if(!state_ || state_->heap.state()!=memory::ResourceState::open) return std::unexpected(Error{ErrorCode::invalid_state,"empty or closed scene pipeline"});
    try {
        check(validate_render_settings(view,settings)); take(detail::perspective_depth(view.description()));
        { auto guard=take(queue.begin()); check(guard.retain(state_->identity)); }
#ifdef DK_RENDER_CLOUDS
        CloudCompositionSettings composition; composition.reference.time_seconds=environment.time_seconds;
        CloudOptimizedSettings optimized; optimized.composition.reference.time_seconds=environment.time_seconds;
        optimized.view_id=environment.view_id; optimized.camera_revision=environment.camera_revision;
        if(cloudy) {
            if(environment.mode==CloudRenderMode::reference) check(validate_cloud_composition(view.scene().environment(),composition,std::size_t(view.description().width)*view.description().height));
            else check(validate_cloud_optimization(view.scene().environment(),optimized,view.description().width,view.description().height));
        }
        check(poll_cloud_history(queue));
#else
        if(cloudy) throw Error{ErrorCode::not_supported,"cloud composition requires DK_BUILD_RENDER_CLOUDS"};
#endif
        std::optional<AtmosphereRenderer> candidate;
        if(!state_->atmosphere) candidate=take(AtmosphereRenderer::create(state_->heap,queue,state_->directory/"../../engine/render/atmosphere/shaders"));
        auto& renderer=state_->atmosphere ? *state_->atmosphere : *candidate;
        auto sky=take(renderer.prepare(queue,view.scene().environment(),view.description().camera_world.matrix().topRightCorner<3,1>()));
        Result<RenderFrame> frame;
#ifdef DK_RENDER_CLOUDS
        std::unique_ptr<detail::CloudEnvironment> cloud_candidate;
        if(cloudy) {
            if(!state_->clouds) { cloud_candidate=std::make_unique<detail::CloudEnvironment>(); cloud_candidate->textures=take(CloudTextureCache::create(queue)); }
            auto& owner=state_->clouds ? *state_->clouds : *cloud_candidate;
            const auto data=take(owner.fields.prepare(view.scene().environment().clouds));
            const auto textures=take(owner.textures->prepare(queue,*data));
            CloudShadowMap shadow;
            if(environment.cloud_shadows && view.scene().environment().enabled) {
                if(!owner.shadows) owner.shadows=take(CloudShadowRenderer::create(state_->heap,queue,state_->directory/"../../engine/render/clouds/shaders"));
                CloudShadowSettings options; options.time_seconds=environment.time_seconds; options.center_m=environment.shadow_center_m;
                options.extent_m=environment.shadow_extent_m; options.resolution=environment.shadow_resolution;
                shadow=take(owner.shadows->prepare(queue,view.scene().environment(),*textures,options));
            }
            if(environment.mode==CloudRenderMode::reference) frame=render_clouds(queue,view,assets,sky,*textures,composition,settings,shadow ? &shadow : nullptr);
            else frame=render_clouds_optimized(queue,view,assets,sky,*textures,optimized,settings,shadow ? &shadow : nullptr);
        } else
#endif
        frame=render_atmosphere(queue,view,assets,sky,settings);
        if(frame && candidate) state_->atmosphere=std::move(candidate);
#ifdef DK_RENDER_CLOUDS
        if(frame && cloud_candidate) state_->clouds=std::move(cloud_candidate);
        if(frame && (environment.mode==CloudRenderMode::reference || !cloudy))
            for(auto& slot:state_->cloud_history) if(slot.id==environment.view_id) slot={};
#endif
        return frame;
    } catch(const Error& error) { return std::unexpected(error); }
    catch(const std::bad_alloc&) { return std::unexpected(allocation_error()); }
#else
    return std::unexpected(Error{ErrorCode::not_supported,"environment rendering requires DK_BUILD_RENDER_ATMOSPHERE"});
#endif
}
Result<RenderFrame> ScenePipeline::render_clouds(SubmissionQueue& queue,const RenderView& view,const GpuAssets& assets,
    const AtmosphereSky& sky,const CloudTextures& textures,const CloudCompositionSettings& clouds,const RenderSettings& settings,const CloudShadowMap* shadow) {
#ifdef DK_RENDER_CLOUDS
    if(!active_clouds(view.scene().environment())) return render_atmosphere(queue,view,assets,sky,settings,{clouds.surface_lighting,clouds.sun_disk,false},shadow);
    if(auto valid=validate_cloud_composition(view.scene().environment(),clouds,std::size_t(view.description().width)*view.description().height); !valid)
        return std::unexpected(valid.error());
    const AtmosphereSettings atmosphere{clouds.surface_lighting && view.scene().environment().enabled,clouds.sun_disk,true};
    return render_impl(queue,view,assets,settings,&sky,&atmosphere,&textures,&clouds,nullptr,shadow);
#else
    static_cast<void>(queue); static_cast<void>(view); static_cast<void>(assets); static_cast<void>(sky);
    static_cast<void>(textures); static_cast<void>(clouds); static_cast<void>(settings); static_cast<void>(shadow);
    return std::unexpected(Error{ErrorCode::not_supported,"cloud composition requires Clouds and Atmosphere"});
#endif
}
Result<RenderFrame> ScenePipeline::render_clouds_optimized(SubmissionQueue& queue,const RenderView& view,const GpuAssets& assets,
    const AtmosphereSky& sky,const CloudTextures& textures,const CloudOptimizedSettings& optimized,const RenderSettings& settings,const CloudShadowMap* shadow) {
#ifdef DK_RENDER_CLOUDS
    if(auto valid=validate_cloud_optimization(view.scene().environment(),optimized,view.description().width,view.description().height); !valid)
        return std::unexpected(valid.error());
    if(!active_clouds(view.scene().environment())) {
        if(auto ready=poll_cloud_history(queue); !ready) return std::unexpected(ready.error());
        for(const auto& slot:state_->cloud_history) if(slot.id==optimized.view_id && slot.pending)
            return std::unexpected(Error{ErrorCode::invalid_state,"Cloud view still has an in-flight candidate"});
        auto frame=render_atmosphere(queue,view,assets,sky,settings,{optimized.composition.surface_lighting,optimized.composition.sun_disk,false},shadow);
        if(frame) for(auto& slot:state_->cloud_history) if(slot.id==optimized.view_id) { slot.completed.reset(); slot.failed=false; }
        return frame;
    }
    const AtmosphereSettings atmosphere{optimized.composition.surface_lighting && view.scene().environment().enabled,optimized.composition.sun_disk,false};
    return render_impl(queue,view,assets,settings,&sky,&atmosphere,&textures,&optimized.composition,&optimized,shadow);
#else
    static_cast<void>(queue); static_cast<void>(view); static_cast<void>(assets); static_cast<void>(sky);
    static_cast<void>(textures); static_cast<void>(optimized); static_cast<void>(settings); static_cast<void>(shadow);
    return std::unexpected(Error{ErrorCode::not_supported,"Cloud optimization requires Clouds and Atmosphere"});
#endif
}
Result<void> ScenePipeline::poll_cloud_history(SubmissionQueue& queue) {
#ifdef DK_RENDER_CLOUDS
    if(!state_ || state_->heap.state()!=memory::ResourceState::open) return std::unexpected(Error{ErrorCode::invalid_state,"Cloud history owner unavailable"});
    try {
        { auto guard=take(queue.begin()); check(guard.retain(state_->identity)); }
        check(queue.poll()); const auto completed=queue.stats().completed;
        for(auto& slot:state_->cloud_history) if(slot.pending && slot.pending->submission.value()<=completed) slot.completed=std::move(slot.pending);
        return {};
    } catch(const Error& error) { return std::unexpected(error); }
#else
    static_cast<void>(queue); return std::unexpected(Error{ErrorCode::not_supported,"Cloud history unavailable"});
#endif
}
Result<CloudHistoryStatus> ScenePipeline::cloud_history(std::uint64_t view_id) const {
#ifdef DK_RENDER_CLOUDS
    if(state_) for(const auto& slot:state_->cloud_history) if(slot.id==view_id && view_id)
        return CloudHistoryStatus{slot.completed ? slot.completed->sequence : 0,slot.pending ? slot.pending->sequence : 0,slot.completed && !slot.failed};
#else
    static_cast<void>(view_id);
#endif
    return std::unexpected(Error{ErrorCode::not_found,"Cloud history view not found"});
}
Result<void> ScenePipeline::release_cloud_history(SubmissionQueue& queue,std::uint64_t view_id) {
    if(auto ready=poll_cloud_history(queue); !ready) return ready;
#ifdef DK_RENDER_CLOUDS
    for(auto& slot:state_->cloud_history) if(slot.id==view_id && view_id) {
        if(slot.pending) return std::unexpected(Error{ErrorCode::invalid_state,"Cloud view still has an in-flight candidate"});
        slot={}; return {};
    }
#else
    static_cast<void>(view_id);
#endif
    return std::unexpected(Error{ErrorCode::not_found,"Cloud history view not found"});
}
Result<std::string> shader_fingerprint(const std::filesystem::path& directory,bool environment) {
    const std::array files{"scene.slang","tone.slang","sunlight.slang","aerial.slang",
        "../../engine/render/atmosphere/shaders/atmosphere.slang","../../engine/render/atmosphere/shaders/AtmosphereTransport.slang",
        "cloud-composition.slang","cloud-reconstruction.slang","../../engine/render/clouds/shaders/cloud-shadow.slang","../../engine/render/clouds/shaders/CloudTypes.slang","../../engine/render/clouds/shaders/CloudTransport.slang"};
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
    for(const unsigned char c:std::string_view{"render-environment-v2:64x64x128:128:cloud-joint-v1:256x64x64:cloud-reconstruct-v1:96to256x8:half-fixed:cloud-shadow-v1:sunward-inner-sphere:environment-session-v1"}) append(c);
    std::ostringstream out; out<<"fnv1a64:"<<std::hex<<std::setfill('0')<<std::setw(16)<<hash; return out.str();
}
Result<RenderFrame> ScenePipeline::render_atmosphere(SubmissionQueue& queue,const RenderView& view,const GpuAssets& assets,const AtmosphereSky& sky,const RenderSettings& settings,const AtmosphereSettings& atmosphere,const CloudShadowMap* shadow) {
    const bool enabled=view.scene().environment().enabled;
    return render_impl(queue,view,assets,settings,enabled ? &sky : nullptr,enabled ? &atmosphere : nullptr,nullptr,nullptr,nullptr,shadow);
}
Result<RenderFrame> ScenePipeline::render_impl(SubmissionQueue& queue,const RenderView& view,const GpuAssets& assets,const RenderSettings& settings,const AtmosphereSky* sky,const AtmosphereSettings* atmosphere,const CloudTextures* clouds,const CloudCompositionSettings* cloud_settings,const CloudOptimizedSettings* optimized,const CloudShadowMap* shadow) {
    DK_PROFILE_ZONE("render.scene");
    if (!state_ || state_->heap.state()!=memory::ResourceState::open || queue.stats().closed || queue.stats().device_lost)
        return std::unexpected(Error{ErrorCode::invalid_state,"scene pipeline, memory or queue is unavailable"});
    if (auto valid=validate_render_settings(view,settings); !valid) return std::unexpected(valid.error());
    const auto width=view.description().width, height=view.description().height;
    const auto& limits=queue.device().adapter().properties.limits;
    if (width>limits.maxFramebufferWidth || height>limits.maxFramebufferHeight || width>limits.maxViewportDimensions[0] || height>limits.maxViewportDimensions[1])
        return std::unexpected(Error{ErrorCode::not_supported,"render extent exceeds device limits"});
#ifdef DK_RENDER_CLOUDS
    detail::CloudHistorySlot* history_slot=nullptr;
    auto invalidate=[&] { if(history_slot && history_slot->id) history_slot->failed=true; };
#else
    auto invalidate=[] {};
#endif
    try {
        const auto heap=state_->heap;
        { auto guard=take(queue.begin()); check(guard.retain(state_->identity)); }
        if(shadow && (!sky || (atmosphere && !atmosphere->surface_lighting))) throw Error{ErrorCode::invalid_argument,"Cloud shadow requires active surface sunlight"};
#ifdef DK_RENDER_CLOUDS
        if(shadow) {
            if(take(shadow->environment())!=view.scene().environment()) throw Error{ErrorCode::invalid_argument,"Cloud shadow environment mismatch"};
            { auto guard=take(queue.begin()); check(guard.retain(*take(shadow->image()))); }
            if(cloud_settings) {
                const auto shadow_settings=take(shadow->settings()); const auto& reference=cloud_settings->reference;
                if(shadow_settings.time_seconds!=reference.time_seconds || shadow_settings.homogeneous!=reference.homogeneous ||
                    shadow_settings.homogeneous_density!=reference.homogeneous_density)
                    throw Error{ErrorCode::invalid_argument,"Cloud shadow time or density model mismatch"};
            }
        }
#else
        if(shadow) throw Error{ErrorCode::not_supported,"Cloud shadow requires Clouds"};
#endif
        Inputs inputs{heap}; resolve(inputs,view,assets);
        Buffer aerial_uniform,cloud_uniform,reconstruction_uniform;
        CloudFrameInfo cloud_info;
#ifdef DK_RENDER_CLOUDS
        std::shared_ptr<detail::CloudHistoryGeneration> candidate,previous;
        if(optimized) {
            if(optimized->temporal) {
                check(poll_cloud_history(queue));
                auto slot=std::ranges::find(state_->cloud_history,optimized->view_id,&detail::CloudHistorySlot::id);
                if(slot==state_->cloud_history.end()) slot=std::ranges::find(state_->cloud_history,0,&detail::CloudHistorySlot::id);
                if(slot==state_->cloud_history.end()) throw Error{ErrorCode::invalid_state,"Cloud history view budget exceeded"};
                if(slot->pending) throw Error{ErrorCode::invalid_state,"Cloud view still has an in-flight candidate"};
                if(slot->sequence==UINT64_MAX) throw Error{ErrorCode::invalid_state,"Cloud history sequence exhausted"};
                history_slot=&*slot;
            }
            cloud_info.view_id=optimized->view_id; cloud_info.width=(width+1)/2; cloud_info.height=(height+1)/2;
            cloud_info.reset=history_slot ? history_reset(*history_slot,view,*optimized) : CloudHistoryReset::disabled;
            if(cloud_info.reset==CloudHistoryReset::none) { previous=history_slot->completed; cloud_info.history_available=true; cloud_info.source_sequence=previous->sequence; }
            cloud_info.candidate_sequence=history_slot ? history_slot->sequence+1 : 0;
            std::uint64_t bytes=std::uint64_t{cloud_info.width}*cloud_info.height*48;
            for(const auto& slot:state_->cloud_history) for(const auto& generation:{slot.completed,slot.pending}) if(generation)
                bytes+=std::uint64_t{(generation->view.width+1)/2}*((generation->view.height+1)/2)*48;
            if(bytes>256ULL*1024*1024) throw Error{ErrorCode::invalid_state,"Cloud history image budget exceeded"};
            candidate=memory::make_shared_in<detail::CloudHistoryGeneration>(heap); candidate->settings=*optimized; candidate->view=view.description();
            candidate->scene=view.scene().id(); candidate->revision=view.scene().revision(); candidate->environment=view.scene().environment(); candidate->sequence=cloud_info.candidate_sequence;
            for(auto& image:candidate->images) image=take(state_->factory.create_image({cloud_info.width,cloud_info.height,vk::Format::eR32G32B32A32Sfloat,
                vk::ImageUsageFlagBits::eStorage|vk::ImageUsageFlagBits::eSampled}));
            reconstruction_uniform=prepare_reconstruction(*state_,view,*optimized,previous.get());
        }
#endif
        if(sky) {
#ifdef DK_RENDER_SUNLIGHT
            if(atmosphere) aerial_uniform=prepare_aerial(*state_,view,*sky,*atmosphere,clouds!=nullptr);
            if(!atmosphere || atmosphere->surface_lighting) {
                resolve_sunlight(inputs,view,*sky,settings,*state_,shadow);
                prepare_sunlight(*state_);
            }
#else
            return std::unexpected(Error{ErrorCode::not_supported,"environment rendering requires DK_BUILD_RENDER_ATMOSPHERE"});
#endif
        }
#ifdef DK_RENDER_CLOUDS
        if(clouds) cloud_uniform=prepare_clouds(*state_,view,*clouds,*cloud_settings,settings,optimized);
#else
        static_cast<void>(cloud_settings);
#endif
        auto frame=memory::make_shared_in<detail::FrameState>(heap);
        frame->cloud_info=cloud_info;
#ifdef DK_RENDER_CLOUDS
        if(shadow) { frame->shadow=*shadow; frame->shadow_generation=shadow->generation(); }
#endif
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
        graph::ResourceId cloud_constants{}; std::array<graph::ResourceId,2> diagnostic_readbacks{};
        std::size_t cloud_parameters=0;
        graph::ResourceId reconstruction_constants{},statistics_id{},statistics_readback_id{};
        std::size_t reconstruction_parameters=0,statistics_index=0;
#ifdef DK_RENDER_CLOUDS
        if(clouds) {
            cloud_parameters=graph.counts().buffers;
            cloud_constants=take(graph.declare_buffer("cloud parameters",cloud_uniform.description(),graph::Lifetime::external,true));
            external.push_back({cloud_parameters,&cloud_uniform});
            if(cloud_settings->diagnostics) for(unsigned i=0;i<2;++i) {
                frame->cloud_readback[i]=graph.counts().buffers;
                diagnostic_readbacks[i]=take(graph.declare_buffer("cloud diagnostic readback "+std::to_string(i),
                    {vk::DeviceSize{width}*height*16,vk::BufferUsageFlagBits::eTransferDst,BufferMemory::readback}));
            }
        }
        if(optimized) {
            reconstruction_parameters=graph.counts().buffers;
            reconstruction_constants=take(graph.declare_buffer("cloud reconstruction parameters",reconstruction_uniform.description(),graph::Lifetime::external,true));
            external.push_back({reconstruction_parameters,&reconstruction_uniform});
            statistics_index=graph.counts().buffers;
            statistics_id=take(graph.declare_buffer("cloud work statistics",{sizeof(CloudTraceStatistics),
                vk::BufferUsageFlagBits::eStorageBuffer|vk::BufferUsageFlagBits::eTransferDst|vk::BufferUsageFlagBits::eTransferSrc}));
            if(cloud_settings->diagnostics) {
                frame->cloud_statistics=graph.counts().buffers;
                statistics_readback_id=take(graph.declare_buffer("cloud statistics readback",{sizeof(CloudTraceStatistics),vk::BufferUsageFlagBits::eTransferDst,BufferMemory::readback}));
            }
        }
#endif
        const auto image_base=graph.counts().buffers;
        const auto hdr=take(graph.declare_image("linear HDR",{width,height,vk::Format::eR32G32B32A32Sfloat,
            vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eTransferSrc}));
        const auto depth=take(graph.declare_image("scene depth",{width,height,vk::Format::eD32Sfloat,
            vk::ImageUsageFlagBits::eDepthStencilAttachment | vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled}));
        const auto ldr=take(graph.declare_image("sRGB RGBA8",{width,height,vk::Format::eR8G8B8A8Unorm,
            vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled}));
        const auto white=take(graph.declare_image("default white",{1,1}));
        Context context{*state_,inputs,settings,width,height,image_base,image_base+1,image_base+2,image_base+3};
        context.clouds=clouds; context.cloud_parameters=cloud_parameters; context.cloud_readback=frame->cloud_readback;
        context.optimized=optimized!=nullptr; context.reconstruction_parameters=reconstruction_parameters;
        context.statistics=statistics_index; context.statistics_readback=frame->cloud_statistics;
        frame->color=context.ldr; context.linear_readback=frame->linear_readback; context.parameters=parameters_index;
        Vector<graph::Use> clear_uses(0,memory::Allocator<graph::Use>{heap});
        clear_uses={use(hdr,vk::PipelineStageFlagBits2::eClear,vk::AccessFlagBits2::eTransferWrite,vk::ImageLayout::eTransferDstOptimal,true),
            use(depth,vk::PipelineStageFlagBits2::eClear,vk::AccessFlagBits2::eTransferWrite,vk::ImageLayout::eTransferDstOptimal,true,true),
            use(ldr,vk::PipelineStageFlagBits2::eClear,vk::AccessFlagBits2::eTransferWrite,vk::ImageLayout::eTransferDstOptimal,true),
            use(white,vk::PipelineStageFlagBits2::eClear,vk::AccessFlagBits2::eTransferWrite,vk::ImageLayout::eTransferDstOptimal,true)};
        if(optimized) clear_uses.push_back(use(statistics_id,vk::PipelineStageFlagBits2::eClear,vk::AccessFlagBits2::eTransferWrite,vk::ImageLayout::eUndefined,true));
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
                if(clouds && i==2 && !inputs.sky) continue;
                const auto* image=take(sky->image(luts[i]));
                context.atmosphere_luts[i]=graph.counts().buffers+graph.counts().images;
                lut_ids[i]=take(graph.declare_image("atmosphere LUT "+std::to_string(i),image->description(),graph::Lifetime::external,true));
                external.push_back({context.atmosphere_luts[i],nullptr,image,{}});
                if(inputs.sky && i!=1) geometry.push_back(use(lut_ids[i],vk::PipelineStageFlagBits2::eFragmentShader,vk::AccessFlagBits2::eShaderSampledRead,vk::ImageLayout::eShaderReadOnlyOptimal));
            }
        }
#endif
        if(inputs.shadow) {
            const auto index=graph.counts().buffers+graph.counts().images;
            const auto id=take(graph.declare_image("surface sun cloud transmittance",inputs.shadow->description(),graph::Lifetime::external,true));
            external.push_back({index,nullptr,inputs.shadow,{}});
            geometry.push_back(use(id,vk::PipelineStageFlagBits2::eFragmentShader,vk::AccessFlagBits2::eShaderSampledRead,vk::ImageLayout::eShaderReadOnlyOptimal));
        }
        geometry.push_back(use(hdr,vk::PipelineStageFlagBits2::eColorAttachmentOutput,color_access,vk::ImageLayout::eColorAttachmentOptimal));
        static_cast<void>(take(graph.add_pass({"opaque",geometry})));
        auto final_hdr=hdr;
        if(atmosphere && !clouds) {
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
#ifdef DK_RENDER_CLOUDS
        std::array<graph::ResourceId,2> cloud_outputs{};
        if(clouds) {
            constexpr auto compute=vk::PipelineStageFlagBits2::eComputeShader; constexpr auto sampled=vk::AccessFlagBits2::eShaderSampledRead;
            context.composed=graph.counts().buffers+graph.counts().images;
            ImageDesc image{width,height,vk::Format::eR32G32B32A32Sfloat,vk::ImageUsageFlagBits::eStorage|vk::ImageUsageFlagBits::eSampled|vk::ImageUsageFlagBits::eTransferSrc};
            final_hdr=take(graph.declare_image("cloud air surface HDR",image));
            for(unsigned i=0;i<2;++i) {
                context.cloud_images[i]=graph.counts().buffers+graph.counts().images;
                cloud_outputs[i]=take(graph.declare_image(i==0 ? "cloud L and T" : "medium T and cloud distance",image));
            }
            Vector<graph::Use> uses(0,memory::Allocator<graph::Use>{heap});
            uses={use(constants,compute,vk::AccessFlagBits2::eUniformRead),use(cloud_constants,compute,vk::AccessFlagBits2::eUniformRead),
                use(hdr,compute,sampled,vk::ImageLayout::eShaderReadOnlyOptimal),use(depth,compute,sampled,vk::ImageLayout::eShaderReadOnlyOptimal,false,true),
                use(final_hdr,compute,vk::AccessFlagBits2::eShaderStorageWrite,vk::ImageLayout::eGeneral,true)};
            for(unsigned i=0;i<2;++i) {
                uses.push_back(use(lut_ids[i],compute,sampled,vk::ImageLayout::eShaderReadOnlyOptimal));
                uses.push_back(use(cloud_outputs[i],compute,vk::AccessFlagBits2::eShaderStorageWrite,vk::ImageLayout::eGeneral,true));
            }
            std::array<graph::ResourceId,3> fields;
            for(unsigned i=0;i<3;++i) {
                const auto& field=clouds->image(static_cast<CloudField>(i)); const auto index=graph.counts().buffers+graph.counts().images;
                const auto id=take(graph.declare_image("cloud field "+std::to_string(i),field.description(),graph::Lifetime::external,true));
                fields[i]=id; external.push_back({index,nullptr,&field,{}}); uses.push_back(use(id,compute,sampled,vk::ImageLayout::eShaderReadOnlyOptimal));
            }
            if(!optimized) static_cast<void>(take(graph.add_pass({"joint cloud air geometry composition",uses})));
            else {
                std::array<graph::ResourceId,3> raw,filtered,history;
                ImageDesc half{(width+1)/2,(height+1)/2,vk::Format::eR32G32B32A32Sfloat,vk::ImageUsageFlagBits::eStorage|vk::ImageUsageFlagBits::eSampled};
                for(unsigned i=0;i<3;++i) {
                    context.raw_cloud[i]=graph.counts().buffers+graph.counts().images;
                    raw[i]=take(graph.declare_image("current half cloud "+std::to_string(i),half));
                    context.filtered_cloud[i]=graph.counts().buffers+graph.counts().images;
                    filtered[i]=take(graph.declare_image("candidate cloud history "+std::to_string(i),half,graph::Lifetime::external));
                    external.push_back({context.filtered_cloud[i],nullptr,&candidate->images[i],{}});
                    if(previous) {
                        context.previous_cloud[i]=graph.counts().buffers+graph.counts().images;
                        history[i]=take(graph.declare_image("completed cloud history "+std::to_string(i),half,graph::Lifetime::external,true));
                        external.push_back({context.previous_cloud[i],nullptr,&previous->images[i],{}});
                    } else { history[i]=raw[i]; context.previous_cloud[i]=context.raw_cloud[i]; }
                }
                Vector<graph::Use> half_uses(0,memory::Allocator<graph::Use>{heap});
                half_uses={use(constants,compute,vk::AccessFlagBits2::eUniformRead),use(cloud_constants,compute,vk::AccessFlagBits2::eUniformRead),
                    use(reconstruction_constants,compute,vk::AccessFlagBits2::eUniformRead),
                    use(statistics_id,compute,vk::AccessFlagBits2::eShaderStorageRead|vk::AccessFlagBits2::eShaderStorageWrite),
                    use(depth,compute,sampled,vk::ImageLayout::eShaderReadOnlyOptimal,false,true)};
                for(unsigned i=0;i<2;++i) half_uses.push_back(use(lut_ids[i],compute,sampled,vk::ImageLayout::eShaderReadOnlyOptimal));
                for(unsigned i=0;i<3;++i) {
                    half_uses.push_back(use(fields[i],compute,sampled,vk::ImageLayout::eShaderReadOnlyOptimal));
                    half_uses.push_back(use(raw[i],compute,vk::AccessFlagBits2::eShaderStorageWrite,vk::ImageLayout::eGeneral,true));
                }
                static_cast<void>(take(graph.add_pass({"half resolution cloud transport",half_uses})));
                Vector<graph::Use> temporal_uses(0,memory::Allocator<graph::Use>{heap});
                temporal_uses={use(constants,compute,vk::AccessFlagBits2::eUniformRead),use(cloud_constants,compute,vk::AccessFlagBits2::eUniformRead),
                    use(reconstruction_constants,compute,vk::AccessFlagBits2::eUniformRead),
                    use(statistics_id,compute,vk::AccessFlagBits2::eShaderStorageRead|vk::AccessFlagBits2::eShaderStorageWrite)};
                for(unsigned i=0;i<3;++i) {
                    temporal_uses.push_back(use(raw[i],compute,sampled,vk::ImageLayout::eShaderReadOnlyOptimal));
                    if(previous) temporal_uses.push_back(use(history[i],compute,sampled,vk::ImageLayout::eShaderReadOnlyOptimal));
                    temporal_uses.push_back(use(filtered[i],compute,vk::AccessFlagBits2::eShaderStorageWrite,vk::ImageLayout::eGeneral,true));
                }
                static_cast<void>(take(graph.add_pass({"completed cloud history reprojection",temporal_uses})));
                uses.push_back(use(reconstruction_constants,compute,vk::AccessFlagBits2::eUniformRead));
                uses.push_back(use(statistics_id,compute,vk::AccessFlagBits2::eShaderStorageRead|vk::AccessFlagBits2::eShaderStorageWrite));
                for(unsigned i=0;i<3;++i) uses.push_back(use(filtered[i],compute,sampled,vk::ImageLayout::eShaderReadOnlyOptimal));
                static_cast<void>(take(graph.add_pass({"depth preserving cloud reconstruction",uses})));
            }
        }
#endif
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
#ifdef DK_RENDER_CLOUDS
        if(context.cloud_readback[0]) for(unsigned i=0;i<2;++i) {
            read_uses.push_back(use(cloud_outputs[i],vk::PipelineStageFlagBits2::eCopy,vk::AccessFlagBits2::eTransferRead,vk::ImageLayout::eTransferSrcOptimal));
            read_uses.push_back(use(diagnostic_readbacks[i],vk::PipelineStageFlagBits2::eCopy,vk::AccessFlagBits2::eTransferWrite,vk::ImageLayout::eUndefined,true));
            check(graph.mark_output(diagnostic_readbacks[i]));
        }
        if(context.statistics_readback) {
            read_uses.push_back(use(statistics_id,vk::PipelineStageFlagBits2::eCopy,vk::AccessFlagBits2::eTransferRead));
            read_uses.push_back(use(statistics_readback_id,vk::PipelineStageFlagBits2::eCopy,vk::AccessFlagBits2::eTransferWrite,vk::ImageLayout::eUndefined,true));
            check(graph.mark_output(statistics_readback_id));
        }
#endif
        static_cast<void>(take(graph.add_pass({"readback",read_uses})));
        check(graph.mark_output(readback)); check(graph.mark_output(ldr));
        auto plan=take(graph.compile());
        if (settings.capture_plan) frame->report=take(graph::format_plan(plan));
        Vector<graph::PassCallback> callbacks(0,memory::Allocator<graph::PassCallback>{heap});
        callbacks={{0,record<clear_pass>,&context},{1,record<geometry_pass>,&context},{2,record<geometry_pass>,&context}};
        if(atmosphere && !clouds) { callbacks.push_back({3,record<aerial_pass>,&context}); callbacks.push_back({4,record<aerial_pass>,&context}); }
#ifdef DK_RENDER_CLOUDS
        if(clouds && !optimized) callbacks.push_back({3,record<cloud_pass>,&context});
        if(optimized) for(unsigned i=0;i<3;++i) callbacks.push_back({3+i,record<reconstruction_pass>,&context});
#endif
        callbacks.push_back({callbacks.size(),record<tone_pass>,&context}); callbacks.push_back({callbacks.size(),record<copy_pass>,&context});
        Vector<graph::FinalAccess> finals(0,memory::Allocator<graph::FinalAccess>{heap});
        finals={{0,{{vk::PipelineStageFlagBits2::eHost,vk::AccessFlagBits2::eHostRead}}},
            {context.ldr,{{vk::PipelineStageFlagBits2::eFragmentShader,vk::AccessFlagBits2::eShaderSampledRead,vk::ImageLayout::eShaderReadOnlyOptimal}}}};
        if(settings.linear_readback) finals.push_back({context.linear_readback,{{vk::PipelineStageFlagBits2::eHost,vk::AccessFlagBits2::eHostRead}}});
        if(context.cloud_readback[0]) for(unsigned i=0;i<2;++i)
            finals.push_back({context.cloud_readback[i],{{vk::PipelineStageFlagBits2::eHost,vk::AccessFlagBits2::eHostRead}}});
        if(context.statistics_readback) finals.push_back({context.statistics_readback,{{vk::PipelineStageFlagBits2::eHost,vk::AccessFlagBits2::eHostRead}}});
        frame->execution=take(graph::execute(plan,queue,{external,callbacks,finals}));
#ifdef DK_RENDER_CLOUDS
        if(shadow) shadow->used_by(frame->execution.submission());
        if(history_slot) {
            candidate->submission=frame->execution.submission(); history_slot->id=optimized->view_id;
            history_slot->sequence=candidate->sequence; history_slot->pending=std::move(candidate); history_slot->failed=false;
        }
#endif
        return RenderFrame{std::move(frame)};
    } catch (Error& error) { invalidate(); return std::unexpected(std::move(error)); }
    catch (const std::bad_alloc&) { invalidate(); return std::unexpected(allocation_error()); }
}
FrameInfo RenderFrame::info() const noexcept { return state_ ? state_->info : FrameInfo{}; }
std::uint64_t RenderFrame::cloud_shadow_generation() const noexcept { return state_ ? state_->shadow_generation : 0; }
CloudFrameInfo RenderFrame::cloud_optimization() const noexcept { return state_ ? state_->cloud_info : CloudFrameInfo{}; }
Result<CloudTraceStatistics> RenderFrame::read_cloud_statistics() const {
    if(!state_ || !state_->cloud_statistics) return std::unexpected(Error{ErrorCode::invalid_state,"Cloud statistics were not requested"});
    auto buffer=state_->execution.buffer(state_->cloud_statistics); if(!buffer) return std::unexpected(buffer.error());
    CloudTraceStatistics value; static_assert(sizeof(value)==40);
    auto read=(*buffer)->read(0,std::as_writable_bytes(std::span{&value,1})); if(!read) return std::unexpected(read.error()); return value;
}
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
Result<void> RenderFrame::read_cloud_rgba32f(std::span<std::array<float,4>> destination) const {
    if(!state_ || !state_->cloud_readback[0]) return std::unexpected(Error{ErrorCode::invalid_state,"Cloud diagnostics were not generated"});
    if(destination.size()!=std::uint64_t{state_->info.width}*state_->info.height)
        return std::unexpected(Error{ErrorCode::invalid_argument,"Cloud diagnostic pixel count differs"});
    auto buffer=state_->execution.buffer(state_->cloud_readback[0]); if(!buffer) return std::unexpected(buffer.error());
    return (*buffer)->read(0,std::as_writable_bytes(destination));
}
Result<void> RenderFrame::read_medium_rgba32f(std::span<std::array<float,4>> destination) const {
    if(!state_ || !state_->cloud_readback[1]) return std::unexpected(Error{ErrorCode::invalid_state,"Cloud diagnostics were not generated"});
    if(destination.size()!=std::uint64_t{state_->info.width}*state_->info.height)
        return std::unexpected(Error{ErrorCode::invalid_argument,"Cloud diagnostic pixel count differs"});
    auto buffer=state_->execution.buffer(state_->cloud_readback[1]); if(!buffer) return std::unexpected(buffer.error());
    return (*buffer)->read(0,std::as_writable_bytes(destination));
}
Result<const Image*> RenderFrame::color() const {
    if (!state_) return std::unexpected(Error{ErrorCode::invalid_state,"empty render frame"});
    return state_->execution.image(state_->color);
}
std::string_view RenderFrame::plan_text() const noexcept { return state_ ? state_->report.text() : std::string_view{}; }
} // namespace dk::render

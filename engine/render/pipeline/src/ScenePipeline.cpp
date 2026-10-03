#include "PipelinePolicy.hpp"
#include <dk/graphics/GraphExecution.hpp>
#include <dk/graphics/GraphDiagnostics.hpp>
#include <dk/graphics/ShaderCompiler.hpp>
#include <dk/memory/SmartPtr.hpp>
#include <dk/profiling/Profiler.hpp>
#include <algorithm>
#include <new>

namespace dk::render {
using namespace graphics;
namespace detail {
struct PipelineState {
    memory::ResourceHandle heap;
    ResourceFactory factory;
    PipelineLayout depth_layout, opaque_layout, tone_layout;
    GraphicsPipeline depth, opaque, tone;
    Sampler white_sampler;
};
struct FrameState {
    FrameInfo info;
    std::size_t color = 0;
    graph::Execution execution;
    graph::PlanReport report;
};
}
namespace {
template<class T> T take(Result<T> result) { if (!result) throw std::move(result.error()); return std::move(*result); }
void check(Result<void> result) { if (!result) throw std::move(result.error()); }
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
    std::array<float,24> push{};
    std::size_t vertices = 0, indices = 0;
};
struct Inputs {
    explicit Inputs(memory::ResourceHandle heap)
        : draws(0,memory::Allocator<Draw>{heap}), buffers(0,memory::Allocator<const Buffer*>{heap}),
          textures(0,memory::Allocator<const Image*>{heap}) {}
    Vector<Draw> draws;
    Vector<const Buffer*> buffers;
    Vector<const Image*> textures;
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
                std::copy(rows.begin(),rows.end(),draw.push.begin());
                const MaterialData* material = nullptr;
                if (primitive.material) {
                    auto m = std::ranges::find(found->materials(),*primitive.material,&MaterialData::id);
                    if (m == found->materials().end()) throw Error{ErrorCode::invalid_state,"GPU material reference missing"};
                    material = &*m;
                    if (material->alpha_mode != AlphaMode::opaque)
                        throw Error{ErrorCode::not_supported,"minimal scene pipeline supports opaque materials only"};
                    if (material->base_color_texture) {
                        auto t = std::ranges::find(found->textures(),*material->base_color_texture,&GpuTexture::id);
                        if (t == found->textures().end()) throw Error{ErrorCode::invalid_state,"GPU texture reference missing"};
                        draw.texture = &*t;
                        if (std::ranges::find(result.textures,&t->image) == result.textures.end()) result.textures.push_back(&t->image);
                    }
                }
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
    auto dv = take(c.pipeline.factory.create_view(*take(pass.image(c.depth)),{vk::ImageViewType::e2D,depth_range}));
    ImageView cv,white;
    RenderingDesc desc{}; desc.depth.view = &dv; desc.depth.load = vk::AttachmentLoadOp::eLoad;
    if (!depth_only) {
        cv = take(c.pipeline.factory.create_view(*take(pass.image(c.hdr))));
        white = take(c.pipeline.factory.create_view(*take(pass.image(c.white))));
        desc.color.view = &cv; desc.color.load = vk::AttachmentLoadOp::eLoad;
    }
    auto encoder = take(pass.begin_rendering(desc));
    check(encoder.bind_pipeline(depth_only ? c.pipeline.depth : c.pipeline.opaque));
    for (const auto& draw : c.inputs.draws) {
        check(encoder.vertex_buffer(0,*take(pass.buffer(draw.vertices))));
        check(encoder.index_buffer(*take(pass.buffer(draw.indices)),vk::IndexType::eUint32));
        if (depth_only) check(encoder.push_constants(vk::ShaderStageFlagBits::eVertex,0,std::as_bytes(std::span{draw.push})));
        else {
            const auto& view = draw.texture ? draw.texture->view : white;
            const auto& sampler = draw.texture ? draw.texture->sampler : c.pipeline.white_sampler;
            const std::array writes{BindingWrite{0,0,ImageBinding{&view}},BindingWrite{1,0,SamplerBinding{&sampler}}};
            auto bindings = take(c.pipeline.factory.create_bindings(c.pipeline.opaque_layout,0,writes));
            const std::array sets{&bindings}; check(encoder.bind_sets(sets));
            check(encoder.push_constants(vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment,0,std::as_bytes(std::span{draw.push})));
        }
        check(encoder.draw_indexed(draw.primitive->index_count));
    }
    return encoder.end();
}
Result<void> tone_pass(graph::PassContext& pass,void* pointer) {
    auto& c = *static_cast<Context*>(pointer);
    auto input = take(c.pipeline.factory.create_view(*take(pass.image(c.hdr))));
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
template<auto Record> Result<void> record(graph::PassContext& pass,void* context) {
    try { return Record(pass,context); }
    catch (Error& error) { return std::unexpected(std::move(error)); }
}
Result<void> copy_pass(graph::PassContext& pass,void* pointer) {
    const auto& c = *static_cast<Context*>(pointer);
    return pass.copy_to_buffer(c.ldr,0,{0,0,0,0,c.width,c.height});
}
}
Result<void> validate_render_settings(const RenderView& view,const RenderSettings& settings) {
    const auto& desc = view.description();
    if (!view.scene() || !desc.width || !desc.height ||
        std::uint64_t{desc.width}*desc.height > std::numeric_limits<std::size_t>::max()/4 ||
        !std::isfinite(settings.exposure) || settings.exposure < 0 || !settings.clear_rgb.allFinite() || (settings.clear_rgb.array()<0).any())
        return std::unexpected(Error{ErrorCode::invalid_argument,"render requires a scene, addressable extent and finite nonnegative color/exposure"});
    return {};
}
Result<ScenePipeline> ScenePipeline::create(memory::ResourceHandle heap,SubmissionQueue& queue,const std::filesystem::path& directory) {
    if (!heap || heap.state()!=memory::ResourceState::open || queue.stats().closed || queue.stats().device_lost)
        return std::unexpected(Error{ErrorCode::invalid_state,"scene pipeline requires open memory and queue"});
    try {
        auto state = memory::make_shared_in<detail::PipelineState>(heap); state->heap = heap; state->factory = queue.resources();
        auto compile = [&](const char* file,const char* entry,ShaderStage stage,std::string_view define={}) {
            const std::array definitions{ShaderDefine{define,"1"}};
            auto shader = take(compile_shader({directory/file,entry,stage,{},define.empty() ? std::span<const ShaderDefine>{} : std::span<const ShaderDefine>{definitions}},heap));
            return take(state->factory.create_shader(shader));
        };
        auto opaque_vs = compile("scene.slang","vertexMain",ShaderStage::vertex,"DK_VERTEX");
        auto opaque_fs = compile("scene.slang","fragmentMain",ShaderStage::fragment);
        auto tone_vs = compile("tone.slang","vertexMain",ShaderStage::vertex,"DK_VERTEX");
        auto tone_fs = compile("tone.slang","fragmentMain",ShaderStage::fragment);
        const std::array ds{&opaque_vs};
        const std::array os{&opaque_vs,&opaque_fs};
        const std::array ts{&tone_vs,&tone_fs};
        state->depth_layout = take(state->factory.create_pipeline_layout(ds));
        state->opaque_layout = take(state->factory.create_pipeline_layout(os));
        state->tone_layout = take(state->factory.create_pipeline_layout(ts));
        const std::array vb{vk::VertexInputBindingDescription{0,sizeof(GpuVertex),vk::VertexInputRate::eVertex}};
        const std::array va{vk::VertexInputAttributeDescription{0,0,vk::Format::eR32G32B32Sfloat,offsetof(GpuVertex,position)},
            vk::VertexInputAttributeDescription{1,0,vk::Format::eR32G32Sfloat,offsetof(GpuVertex,uv)}};
        GraphicsPipelineDesc depth{}; depth.vertex = &opaque_vs; depth.layout = &state->depth_layout;
        depth.vertex_bindings = vb; depth.vertex_attributes = va;
        depth.color_format = vk::Format::eUndefined; depth.depth_format = vk::Format::eD32Sfloat; depth.depth_test = depth.depth_write = true;
        state->depth = take(state->factory.create_graphics_pipeline(depth));
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
            geometry.push_back(use(id,vertex ? vk::PipelineStageFlagBits2::eVertexAttributeInput : vk::PipelineStageFlagBits2::eIndexInput,
                vertex ? vk::AccessFlagBits2::eVertexAttributeRead : vk::AccessFlagBits2::eIndexRead));
            external.push_back({i+1,buffer,nullptr,{}});
        }
        const auto image_base=graph.counts().buffers;
        const auto hdr=take(graph.declare_image("linear HDR",{width,height,vk::Format::eR32G32B32A32Sfloat,
            vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst}));
        const auto depth=take(graph.declare_image("scene depth",{width,height,vk::Format::eD32Sfloat,
            vk::ImageUsageFlagBits::eDepthStencilAttachment | vk::ImageUsageFlagBits::eTransferDst}));
        const auto ldr=take(graph.declare_image("sRGB RGBA8",{width,height,vk::Format::eR8G8B8A8Unorm,
            vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled}));
        const auto white=take(graph.declare_image("default white",{1,1}));
        Context context{*state_,inputs,settings,width,height,image_base,image_base+1,image_base+2,image_base+3};
        frame->color=context.ldr;
        const std::array clear_uses{use(hdr,vk::PipelineStageFlagBits2::eClear,vk::AccessFlagBits2::eTransferWrite,vk::ImageLayout::eTransferDstOptimal,true),
            use(depth,vk::PipelineStageFlagBits2::eClear,vk::AccessFlagBits2::eTransferWrite,vk::ImageLayout::eTransferDstOptimal,true,true),
            use(ldr,vk::PipelineStageFlagBits2::eClear,vk::AccessFlagBits2::eTransferWrite,vk::ImageLayout::eTransferDstOptimal,true),
            use(white,vk::PipelineStageFlagBits2::eClear,vk::AccessFlagBits2::eTransferWrite,vk::ImageLayout::eTransferDstOptimal,true)};
        static_cast<void>(take(graph.add_pass({"clear targets",clear_uses})));
        geometry.push_back(use(depth,depth_stages,depth_access,vk::ImageLayout::eDepthStencilAttachmentOptimal,false,true));
        static_cast<void>(take(graph.add_pass({"depth prepass",geometry})));
        geometry.push_back(use(hdr,vk::PipelineStageFlagBits2::eColorAttachmentOutput,color_access,vk::ImageLayout::eColorAttachmentOptimal));
        geometry.push_back(use(white,vk::PipelineStageFlagBits2::eFragmentShader,vk::AccessFlagBits2::eShaderSampledRead,vk::ImageLayout::eShaderReadOnlyOptimal));
        for (std::size_t i=0;i<inputs.textures.size();++i) {
            const auto* texture=inputs.textures[i];
            const auto id=take(graph.declare_image("base-texture-"+std::to_string(i),texture->description(),graph::Lifetime::external,true));
            external.push_back({image_base+4+i,nullptr,texture,{}});
            geometry.push_back(use(id,vk::PipelineStageFlagBits2::eFragmentShader,vk::AccessFlagBits2::eShaderSampledRead,vk::ImageLayout::eShaderReadOnlyOptimal));
        }
        static_cast<void>(take(graph.add_pass({"opaque",geometry})));
        const std::array tone_uses{use(hdr,vk::PipelineStageFlagBits2::eFragmentShader,vk::AccessFlagBits2::eShaderSampledRead,vk::ImageLayout::eShaderReadOnlyOptimal),
            use(ldr,vk::PipelineStageFlagBits2::eColorAttachmentOutput,color_access,vk::ImageLayout::eColorAttachmentOptimal)};
        static_cast<void>(take(graph.add_pass({"tone mapping",tone_uses})));
        const std::array read_uses{use(ldr,vk::PipelineStageFlagBits2::eCopy,vk::AccessFlagBits2::eTransferRead,vk::ImageLayout::eTransferSrcOptimal),
            use(readback,vk::PipelineStageFlagBits2::eCopy,vk::AccessFlagBits2::eTransferWrite,vk::ImageLayout::eUndefined,true)};
        static_cast<void>(take(graph.add_pass({"readback",read_uses})));
        check(graph.mark_output(readback)); check(graph.mark_output(ldr));
        auto plan=take(graph.compile());
        if (settings.capture_plan) frame->report=take(graph::format_plan(plan));
        const std::array callbacks{graph::PassCallback{0,record<clear_pass>,&context},graph::PassCallback{1,record<geometry_pass>,&context},
            graph::PassCallback{2,record<geometry_pass>,&context},graph::PassCallback{3,record<tone_pass>,&context},graph::PassCallback{4,copy_pass,&context}};
        const std::array finals{graph::FinalAccess{0,{{vk::PipelineStageFlagBits2::eHost,vk::AccessFlagBits2::eHostRead}}},
            graph::FinalAccess{context.ldr,{{vk::PipelineStageFlagBits2::eFragmentShader,vk::AccessFlagBits2::eShaderSampledRead,vk::ImageLayout::eShaderReadOnlyOptimal}}}};
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
Result<const Image*> RenderFrame::color() const {
    if (!state_) return std::unexpected(Error{ErrorCode::invalid_state,"empty render frame"});
    return state_->execution.image(state_->color);
}
std::string_view RenderFrame::plan_text() const noexcept { return state_ ? state_->report.text() : std::string_view{}; }
} // namespace dk::render

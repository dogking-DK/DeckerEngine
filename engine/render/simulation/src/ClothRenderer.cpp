#include <dk/render/ClothRenderer.hpp>
#include <dk/graphics/ShaderCompiler.hpp>
#include <dk/memory/SmartPtr.hpp>
#include <algorithm>
#include <cmath>

namespace dk::render {
using namespace graphics;
namespace detail {
struct ClothRendererState {
    memory::ResourceHandle heap;
    ResourceFactory factory;
    PipelineLayout layout;
    GraphicsPipeline pipeline;
};
}
namespace {
template<class T> T take(Result<T> result) { if (!result) throw std::move(result.error()); return std::move(*result); }
void check(Result<void> result) { if (!result) throw std::move(result.error()); }
struct Context {
    dk::render::detail::ClothRendererState& renderer;
    const ClothView& view;
    std::size_t positions=0,color=0,depth=0,readback=0;
};
struct Parameters { std::array<float,16> matrix; std::uint32_t columns,rows; float floor; std::uint32_t unused=0; };
static_assert(sizeof(Parameters)==80);
Result<void> draw(graph::PassContext& pass,void* pointer) {
    try {
        auto& c=*static_cast<Context*>(pointer);
        auto color=take(c.renderer.factory.create_view(*take(pass.image(c.color))));
        ImageViewDesc depth_desc; depth_desc.range.aspectMask=vk::ImageAspectFlagBits::eDepth;
        auto depth=take(c.renderer.factory.create_view(*take(pass.image(c.depth)),depth_desc));
        const std::array writes{BindingWrite{0,0,BufferBinding{take(pass.buffer(c.positions))}}};
        auto bindings=take(c.renderer.factory.create_bindings(c.renderer.layout,0,writes));
        RenderingDesc rendering; rendering.color.view=&color; rendering.color.clear={0.025f,0.035f,0.06f,1}; rendering.depth.view=&depth;
        rendering.color.load=vk::AttachmentLoadOp::eLoad;
        rendering.depth.load=vk::AttachmentLoadOp::eLoad;
        auto encoder=take(pass.begin_rendering(rendering));
        check(encoder.bind_pipeline(c.renderer.pipeline));
        const std::array sets{&bindings}; check(encoder.bind_sets(sets));
        const Parameters parameters{c.view.view_projection,c.view.columns,c.view.rows,c.view.floor_y};
        check(encoder.push_constants(vk::ShaderStageFlagBits::eVertex,0,std::as_bytes(std::span{&parameters,1})));
        check(encoder.viewport({0,0,static_cast<float>(c.view.width),static_cast<float>(c.view.height),0,1}));
        check(encoder.scissor({{0,0},{c.view.width,c.view.height}}));
        check(encoder.draw(6+(c.view.columns-1)*(c.view.rows-1)*6));
        return encoder.end();
    } catch (Error& error) { return std::unexpected(std::move(error)); }
}
Result<void> append(GpuParticleGraph& output,void* pointer) {
    try {
        auto& c=*static_cast<Context*>(pointer); auto& graph=output.graph;
        c.positions=output.position_resource;
        graph::BufferId readback;
        if (c.view.image_readback) {
            c.readback=graph.counts().buffers;
            readback=take(graph.declare_buffer("cloth RGBA8 readback",{vk::DeviceSize{c.view.width}*c.view.height*4,
                vk::BufferUsageFlagBits::eTransferDst,BufferMemory::readback}));
            check(graph.mark_output(readback));
        }
        c.color=graph.counts().buffers+graph.counts().images; c.depth=c.color+1;
        const auto color=take(graph.declare_image("cloth RGBA8",{c.view.width,c.view.height,vk::Format::eR8G8B8A8Unorm,
            vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled}));
        const auto depth=take(graph.declare_image("cloth depth",{c.view.width,c.view.height,vk::Format::eD32Sfloat,
            vk::ImageUsageFlagBits::eDepthStencilAttachment | vk::ImageUsageFlagBits::eTransferDst}));
        const std::array clear_uses{graph::Use{depth,{{vk::PipelineStageFlagBits2::eClear,vk::AccessFlagBits2::eTransferWrite,vk::ImageLayout::eTransferDstOptimal},
            0,VK_WHOLE_SIZE,{vk::ImageAspectFlagBits::eDepth,0,1,0,1},true}},
            graph::Use{color,{{vk::PipelineStageFlagBits2::eClear,vk::AccessFlagBits2::eTransferWrite,vk::ImageLayout::eTransferDstOptimal},
                0,VK_WHOLE_SIZE,{vk::ImageAspectFlagBits::eColor,0,1,0,1},true}}};
        auto index=graph.counts().passes;
        static_cast<void>(take(graph.add_pass({"cloth clear targets",clear_uses})));
        output.callbacks.push_back({index,[](graph::PassContext& pass,void* data) {
            const auto& context=*static_cast<Context*>(data);
            auto clear=pass.clear(context.color,vk::ClearColorValue{std::array<float,4>{0.025f,0.035f,0.06f,1}},{vk::ImageAspectFlagBits::eColor,0,1,0,1});
            if (!clear) return clear;
            return pass.clear_depth(context.depth,1,{vk::ImageAspectFlagBits::eDepth,0,1,0,1});
        },&c});
        const std::array uses{graph::Use{output.positions,{{vk::PipelineStageFlagBits2::eVertexShader,vk::AccessFlagBits2::eShaderStorageRead}}},
            graph::Use{color,{{vk::PipelineStageFlagBits2::eColorAttachmentOutput,vk::AccessFlagBits2::eColorAttachmentRead|vk::AccessFlagBits2::eColorAttachmentWrite,vk::ImageLayout::eColorAttachmentOptimal},
                0,VK_WHOLE_SIZE,{vk::ImageAspectFlagBits::eColor,0,1,0,1},false}},
            graph::Use{depth,{{vk::PipelineStageFlagBits2::eEarlyFragmentTests|vk::PipelineStageFlagBits2::eLateFragmentTests,
                vk::AccessFlagBits2::eDepthStencilAttachmentRead|vk::AccessFlagBits2::eDepthStencilAttachmentWrite,vk::ImageLayout::eDepthStencilAttachmentOptimal},
                0,VK_WHOLE_SIZE,{vk::ImageAspectFlagBits::eDepth,0,1,0,1},false}}};
        index=graph.counts().passes;
        static_cast<void>(take(graph.add_pass({"cloth visualization",uses})));
        output.callbacks.push_back({index,draw,&c}); check(graph.mark_output(color));
        if (c.view.image_readback) {
            const std::array copy_uses{graph::Use{color,{{vk::PipelineStageFlagBits2::eCopy,vk::AccessFlagBits2::eTransferRead,vk::ImageLayout::eTransferSrcOptimal}}},
                graph::Use{readback,{{vk::PipelineStageFlagBits2::eCopy,vk::AccessFlagBits2::eTransferWrite},0,VK_WHOLE_SIZE,{},true}}};
            index=graph.counts().passes;
            static_cast<void>(take(graph.add_pass({"cloth image readback",copy_uses})));
            output.callbacks.push_back({index,[](graph::PassContext& pass,void* data) {
                const auto& context=*static_cast<Context*>(data);
                return pass.copy_to_buffer(context.color,context.readback,{0,0,0,0,context.view.width,context.view.height});
            },&c});
            output.final_accesses.push_back({c.readback,{{vk::PipelineStageFlagBits2::eHost,vk::AccessFlagBits2::eHostRead}}});
        }
        output.final_accesses.push_back({c.color,{{vk::PipelineStageFlagBits2::eFragmentShader,vk::AccessFlagBits2::eShaderSampledRead,vk::ImageLayout::eShaderReadOnlyOptimal}}});
        return {};
    } catch (Error& error) { return std::unexpected(std::move(error)); }
}
}
Result<ClothRenderer> ClothRenderer::create(memory::ResourceHandle heap,SubmissionQueue& queue,const std::filesystem::path& shader) {
    if (!heap || heap.state()!=memory::ResourceState::open || queue.stats().closed || queue.stats().device_lost)
        return std::unexpected(Error{ErrorCode::invalid_state,"cloth renderer requires open heap and queue"});
    try {
        auto state=memory::make_shared_in<dk::render::detail::ClothRendererState>(heap); state->heap=heap; state->factory=queue.resources();
        const std::array defines{ShaderDefine{"DK_VERTEX","1"}};
        auto vs=take(state->factory.create_shader(take(compile_shader({shader,"vertexMain",ShaderStage::vertex,{},defines},heap))));
        auto fs=take(state->factory.create_shader(take(compile_shader({shader,"fragmentMain",ShaderStage::fragment},heap))));
        const std::array shaders{&vs,&fs}; state->layout=take(state->factory.create_pipeline_layout(shaders));
        GraphicsPipelineDesc pipeline{&vs,&fs,&state->layout}; pipeline.depth_format=vk::Format::eD32Sfloat;
        pipeline.depth_test=pipeline.depth_write=true;
        state->pipeline=take(state->factory.create_graphics_pipeline(pipeline));
        return ClothRenderer{std::move(state)};
    } catch (Error& error) { return std::unexpected(std::move(error)); }
    catch (const std::bad_alloc&) { return std::unexpected(Error{ErrorCode::internal_error,"cloth renderer allocation failed"}); }
}
Result<SimulationFrame> ClothRenderer::render(SubmissionQueue& queue,GpuXpbdSolver& solver,std::int64_t dt,
    std::uint32_t count,const ClothView& view,GpuXpbdOptions options) {
    if (!state_ || state_->heap.state()!=memory::ResourceState::open || queue.stats().closed || queue.stats().device_lost)
        return std::unexpected(Error{ErrorCode::invalid_state,"cloth renderer, heap or queue unavailable"});
    if (view.columns<2 || view.columns>32 || view.rows<2 || view.rows>32 || view.columns*view.rows!=solver.particle_count() ||
        !view.width || !view.height || view.width>4096 || view.height>4096 || !std::isfinite(view.floor_y) ||
        !std::ranges::all_of(view.view_projection,[](float x){return std::isfinite(x);}))
        return std::unexpected(Error{ErrorCode::invalid_argument,"cloth requires matching grid, finite camera/floor and extent 1..4096"});
    const auto& limits=queue.device().adapter().properties.limits;
    if (view.width>limits.maxFramebufferWidth || view.height>limits.maxFramebufferHeight ||
        view.width>limits.maxViewportDimensions[0] || view.height>limits.maxViewportDimensions[1])
        return std::unexpected(Error{ErrorCode::not_supported,"cloth extent exceeds device limits"});
    Context context{*state_,view};
    auto physics=solver.advance(queue,dt,count,options,{append,&context});
    if (!physics) return std::unexpected(physics.error());
    SimulationFrame frame; frame.physics_=std::move(*physics); frame.color_=context.color; frame.readback_=context.readback;
    frame.width_=view.width; frame.height_=view.height; frame.has_readback_=view.image_readback;
    return frame;
}
Result<const Image*> SimulationFrame::color() const {
    if (!physics_ && !cpu_) return std::unexpected(Error{ErrorCode::invalid_state,"empty simulation frame"});
    return execution()->image(color_);
}
Result<void> SimulationFrame::read_rgba8(std::span<std::byte> bytes) const {
    if ((!physics_ && !cpu_) || !has_readback_) return std::unexpected(Error{ErrorCode::invalid_state,"image readback was not requested"});
    if (bytes.size()!=std::uint64_t{width_}*height_*4) return std::unexpected(Error{ErrorCode::invalid_argument,"RGBA8 size mismatch"});
    auto buffer=execution()->buffer(readback_); if (!buffer) return std::unexpected(buffer.error());
    return (*buffer)->read(0,bytes);
}
Result<SimulationFrame> ClothRenderer::render_positions(SubmissionQueue& queue,std::span<const ParticlePosition> positions,const ClothView& view) {
    if (!state_ || state_->heap.state()!=memory::ResourceState::open || queue.stats().closed || queue.stats().device_lost)
        return std::unexpected(Error{ErrorCode::invalid_state,"cloth renderer unavailable"});
    if (view.columns<2 || view.columns>32 || view.rows<2 || view.rows>32 || view.columns*view.rows!=positions.size() ||
        !view.width || !view.height || view.width>4096 || view.height>4096 || !std::isfinite(view.floor_y) ||
        !std::ranges::all_of(view.view_projection,[](float x){return std::isfinite(x);}) ||
        !std::ranges::all_of(positions,[](const auto& p){return std::isfinite(p.x)&&std::isfinite(p.y)&&std::isfinite(p.z);}))
        return std::unexpected(Error{ErrorCode::invalid_argument,"Invalid CPU cloth positions or view"});
    const auto& limits=queue.device().adapter().properties.limits;
    if (view.width>limits.maxFramebufferWidth || view.height>limits.maxFramebufferHeight ||
        view.width>limits.maxViewportDimensions[0] || view.height>limits.maxViewportDimensions[1])
        return std::unexpected(Error{ErrorCode::not_supported,"cloth extent exceeds device limits"});
    try {
        auto graph=take(graph::Graph::create(state_->heap));
        auto upload=take(queue.create_buffer({positions.size_bytes(),vk::BufferUsageFlagBits::eTransferSrc,BufferMemory::upload}));
        check(upload.write(0,std::as_bytes(positions)));
        const auto staging=take(graph.declare_buffer("CPU positions upload",upload.description(),graph::Lifetime::external,true));
        const auto buffer=take(graph.declare_buffer("CPU particle positions",{positions.size_bytes(),
            vk::BufferUsageFlagBits::eTransferDst|vk::BufferUsageFlagBits::eStorageBuffer}));
        const std::array uses{graph::Use{staging,{{vk::PipelineStageFlagBits2::eCopy,vk::AccessFlagBits2::eTransferRead}}},
            graph::Use{buffer,{{vk::PipelineStageFlagBits2::eCopy,vk::AccessFlagBits2::eTransferWrite},0,VK_WHOLE_SIZE,{},true}}};
        static_cast<void>(take(graph.add_pass({"CPU cloth upload",uses})));
        const auto bytes=positions.size_bytes();
        std::vector<graph::PassCallback> callbacks{{0,[](graph::PassContext& pass,void* data) {
            return pass.copy_buffer(0,1,*static_cast<const std::size_t*>(data));
        },const_cast<std::size_t*>(&bytes)}};
        std::vector<graph::FinalAccess> finals;
        Context context{*state_,view};
        GpuParticleGraph output{graph,buffer,1,static_cast<std::uint32_t>(positions.size()),callbacks,finals};
        check(append(output,&context));
        const auto plan=take(graph.compile());
        const std::array bindings{graph::ExternalBinding{0,&upload}};
        SimulationFrame frame;
        frame.cpu_=take(graph::execute(plan,queue,{bindings,callbacks,finals}));
        frame.color_=context.color; frame.readback_=context.readback;
        frame.width_=view.width; frame.height_=view.height; frame.has_readback_=view.image_readback;
        return frame;
    } catch (Error& error) { return std::unexpected(std::move(error)); }
}
}

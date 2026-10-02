#include <dk/graphics/Presentation.hpp>
#include <dk/graphics/ShaderCompiler.hpp>
#include <dk/memory/MemorySystem.hpp>
#include <charconv>
#include <cstdio>
#include <stdexcept>
#include <thread>

using namespace dk;
using namespace dk::graphics;
namespace {
void take(Result<void>&& result) { if (!result) throw std::runtime_error(result.error().message); }
template<class T> T take(Result<T>&& result) {
    if (!result) throw std::runtime_error(result.error().message);
    return std::move(*result);
}
GraphicsPipeline create_pipeline(ResourceFactory factory, const CompiledShader& vertex, const CompiledShader& fragment, vk::Format format) {
    auto vs = take(factory.create_shader(vertex));
    auto fs = take(factory.create_shader(fragment));
    const std::array stages{&vs,&fs};
    auto layout = take(factory.create_pipeline_layout(stages));
    GraphicsPipelineDesc desc;
    desc.vertex = &vs; desc.fragment = &fs; desc.layout = &layout; desc.color_format = format;
    return take(factory.create_graphics_pipeline(desc));
}
}
int main(int argc, char** argv)
{
    std::uint32_t frame_limit = 0;
    DeviceOptions options;
    for (int i = 1; i < argc; ++i) {
        const std::string_view argument = argv[i];
        if (argument == "--validation") options.validation = ValidationMode::required;
        else if (argument == "--frames" && i + 1 < argc) {
            const std::string_view count = argv[++i];
            const auto result = std::from_chars(count.data(),count.data()+count.size(),frame_limit);
            if (result.ec != std::errc{} || result.ptr != count.data()+count.size() || !frame_limit || frame_limit > 1'000'000) {
                std::fputs("--frames requires 1..1000000\n",stderr); return 2;
            }
        } else {
            std::fputs("Usage: dk-presentation-demo [--validation] [--frames N]\n",stderr);
            return argument == "--help" ? 0 : 2;
        }
    }
    auto system = memory::MemorySystem::create();
    if (!system) return 1;
    auto heap = system->create_heap({"presentation-demo",memory::DomainCategory::render});
    if (!heap) return 1;
    try {
        {
            auto window = take(platform::Window::create(*heap,{"DeckerEngine - Vulkan triangle",960,640}));
            auto created = Presenter::create(*heap,window,options);
            if (!created) {
                std::fprintf(stderr,"%s\n",created.error().message.c_str());
                for (const auto& context : created.error().context) std::fprintf(stderr,"%s\n",context.c_str());
                return created.error().code == ErrorCode::not_supported || created.error().code == ErrorCode::not_found ? 77 : 1;
            }
            auto presenter = std::move(*created);
            const auto source = std::filesystem::path{DK_COMMON_SHADER_DIR}/"triangle.slang";
            auto vertex = take(compile_shader({source,"vertexMain",ShaderStage::vertex},*heap));
            auto fragment = take(compile_shader({source,"fragmentMain",ShaderStage::fragment},*heap));
            GraphicsPipeline graphics;
            vk::Format format = vk::Format::eUndefined;
            std::uint32_t frames = 0;
            while (!frame_limit || frames < frame_limit) {
                if (take(window.poll_events()).close_requested) break;
                auto acquired = take(presenter.acquire());
                if (acquired.status != AcquireStatus::ready) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(16));
                    continue;
                }
                auto& frame = acquired.frame;
                if (format != frame.format()) {
                    format = frame.format();
                    graphics = create_pipeline(presenter.queue().resources(),vertex,fragment,format);
                }
                const std::array uses{image_use(frame.color(),vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                    vk::AccessFlagBits2::eColorAttachmentRead | vk::AccessFlagBits2::eColorAttachmentWrite,
                    vk::ImageLayout::eColorAttachmentOptimal)};
                take(frame.commands().prepare(uses));
                RenderingDesc rendering;
                rendering.color.view = &frame.color_view();
                auto encoder = take(frame.commands().begin_rendering(rendering));
                take(encoder.bind_pipeline(graphics));
                take(encoder.draw(3));
                take(encoder.end());
                take(presenter.present(std::move(frame)));
                ++frames;
            }
            graphics = {};
            take(presenter.close());
            std::printf("presented=%llu generations=%llu errors=%llu warnings=%llu\n",
                static_cast<unsigned long long>(presenter.stats().presented),static_cast<unsigned long long>(presenter.stats().generation),
                static_cast<unsigned long long>(presenter.queue().device().validation_errors()),
                static_cast<unsigned long long>(presenter.queue().device().validation_warnings()));
            if (presenter.queue().device().validation_errors() ||
                (options.validation == ValidationMode::required && presenter.queue().device().validation_warnings())) return 1;
        }
        if (heap->snapshot().live_allocations || !system->try_close().closed()) return 1;
    } catch (const std::exception& error) { std::fprintf(stderr,"%s\n",error.what()); return 1; }
    return 0;
}

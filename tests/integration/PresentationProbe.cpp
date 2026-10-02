#include <dk/graphics/Presentation.hpp>
#include <dk/graphics/ShaderCompiler.hpp>
#include <dk/graphics/Transfer.hpp>
#include <dk/memory/MemorySystem.hpp>
#include <atomic>
#include <cstdio>
#include <stdexcept>
#include <thread>

using namespace dk;
using namespace dk::graphics;
namespace {
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
void take(Result<void>&& result) { if (!result) throw std::runtime_error(result.error().message); }
template<class T> T take(Result<T>&& result) {
    if (!result) throw std::runtime_error(result.error().message);
    return std::move(*result);
}
struct Diagnostics { std::atomic<unsigned> errors = 0, warnings = 0; };
void diagnostic(void* pointer, const Diagnostic& message) noexcept {
    auto& data = *static_cast<Diagnostics*>(pointer);
    if (message.severity == VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) ++data.errors;
    if (message.severity == VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) ++data.warnings;
    if (message.severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
        std::fprintf(stderr,"%.*s: %.*s\n",static_cast<int>(message.name.size()),message.name.data(),static_cast<int>(message.message.size()),message.message.data());
}
Frame next_frame(Presenter& presenter, const platform::Window& window) {
    for (int i = 0; i < 100; ++i) {
        take(window.poll_events());
        auto acquired = take(presenter.acquire());
        if (acquired.status == AcquireStatus::ready) return std::move(acquired.frame);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    throw std::runtime_error("acquire did not produce a frame");
}
GraphicsPipeline pipeline(ResourceFactory factory, const CompiledShader& vertex, const CompiledShader& fragment, vk::Format format) {
    auto vs = take(factory.create_shader(vertex));
    auto fs = take(factory.create_shader(fragment));
    const std::array modules{&vs,&fs};
    auto layout = take(factory.create_pipeline_layout(modules));
    GraphicsPipelineDesc desc;
    desc.vertex = &vs; desc.fragment = &fs; desc.layout = &layout; desc.color_format = format;
    return take(factory.create_graphics_pipeline(desc));
}
void draw(Frame& frame, const GraphicsPipeline& graphics) {
    const std::array uses{image_use(frame.color(),vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        vk::AccessFlagBits2::eColorAttachmentRead | vk::AccessFlagBits2::eColorAttachmentWrite,vk::ImageLayout::eColorAttachmentOptimal)};
    take(frame.commands().prepare(uses));
    RenderingDesc desc;
    desc.color.view = &frame.color_view();
    auto render = take(frame.commands().begin_rendering(desc));
    take(render.bind_pipeline(graphics));
    take(render.draw(3));
    take(render.end());
}
void verify_pixels(const ReadbackRequest& request, memory::ResourceHandle resource) {
    const auto desc = request.description();
    Vector<std::byte> pixels{memory::Allocator<std::byte>{resource}};
    pixels.resize(static_cast<std::size_t>(desc.bytes));
    require(take(request.try_read(pixels)), "frame readback not ready after completion");
    const auto center = (static_cast<std::size_t>(desc.height / 2) * desc.width + desc.width / 2) * 4;
    require(pixels[0] == std::byte{} && pixels[1] == std::byte{} && pixels[2] == std::byte{} && pixels[3] == std::byte{255}, "presented background mismatch");
    require(std::to_integer<unsigned>(pixels[center]) > 30 && std::to_integer<unsigned>(pixels[center+1]) > 30 &&
        std::to_integer<unsigned>(pixels[center+2]) > 30 && pixels[center+3] == std::byte{255}, "presented triangle center mismatch");
}
void run(Presenter& presenter, const platform::Window& window, memory::ResourceHandle resource,
    const CompiledShader& vertex, const CompiledShader& fragment)
{
    require(!presenter.acquire(UINT64_MAX), "unbounded acquire accepted");
    require(!presenter.present(Frame{}), "empty frame accepted");
    ImageView stale;
    {
        auto frame = next_frame(presenter,window);
        require(!presenter.acquire(), "second active acquire accepted");
        require(!presenter.close(), "close accepted active frame");
        require(!presenter.present(std::move(frame)) && bool(frame), "uninitialized image was presented or consumed");
        auto foreign = take(presenter.queue().begin());
        require(!foreign.retain(frame.color()), "external image admitted to foreign batch");
        require(!presenter.queue().submit(std::move(frame.commands())), "WSI synchronization bypass accepted");
        stale = take(presenter.queue().resources().create_view(frame.color()));
    }
    require(presenter.stats().abandoned == 1, "abandoned acquisition not released");
    {
        auto frame = next_frame(presenter,window);
        require(!frame.commands().retain(stale), "stale external image admitted to new frame");
        auto moved = std::move(frame.commands());
        frame = {};
        require(!presenter.queue().submit(std::move(moved)), "moved-out WSI batch submitted");
    }
    stale = {};
    GraphicsPipeline graphics;
    vk::Format format = vk::Format::eUndefined;
    for (int i = 0; i < 48; ++i) {
        auto frame = next_frame(presenter,window);
        if (format != frame.format()) {
            format = frame.format(); graphics = pipeline(presenter.queue().resources(),vertex,fragment,format);
        }
        draw(frame,graphics);
        ReadbackRequest readback;
        if (i == 0 || i == 47) {
            require(bool(frame.color().description().usage & vk::ImageUsageFlagBits::eTransferSrc), "probe GPU lacks swapchain readback capability");
            readback = take(frame.commands().readback(frame.color(),{0,0,0,0,frame.extent().width,frame.extent().height}));
        }
        if (i == 47) graphics = {}; // Pending batch owns pipeline/layout/modules.
        auto result = take(presenter.present(std::move(frame)));
        require(!frame,"present did not consume frame");
        if (i == 0 || i == 47) {
            require(take(presenter.queue().wait(result.completion)),"render completion wait failed");
            verify_pixels(readback,resource);
        }
    }
    take(presenter.close());
    take(presenter.close());
    require(!presenter.acquire(), "closed presenter accepted frame");
    require(presenter.stats().presented == 48,"present count mismatch");
    VmaTotalStatistics allocations{};
    vmaCalculateStatistics(presenter.queue().device().allocator(),&allocations);
    require(allocations.total.statistics.allocationCount == 0,"presentation VMA allocations leaked");
}
}
int main()
{
    Diagnostics diagnostics;
    auto system = memory::MemorySystem::create(); if (!system) return 1;
    auto heap = system->create_heap({"presentation-probe",memory::DomainCategory::render}); if (!heap) return 1;
    try {
        {
            const auto shader = std::filesystem::path{DK_COMMON_SHADER_DIR}/"triangle.slang";
            auto vertex = take(compile_shader({shader,"vertexMain",ShaderStage::vertex},*heap));
            auto fragment = take(compile_shader({shader,"fragmentMain",ShaderStage::fragment},*heap));
            DeviceOptions options;
            options.validation = ValidationMode::required; options.diagnostic_sink = diagnostic; options.diagnostic_user_data = &diagnostics;
            for (int round = 0; round < 3; ++round) {
                auto window = take(platform::Window::create(*heap,{"DeckerEngine presentation probe",480,320}));
                auto created = Presenter::create(*heap,window,options);
                if (!created) {
                    std::fprintf(stderr,"%s\n",created.error().message.c_str());
                    return created.error().code == ErrorCode::not_supported || created.error().code == ErrorCode::not_found ? 77 : 1;
                }
                auto presenter = std::move(*created);
                std::printf("GPU=%s driver=%s API=%u\n",presenter.queue().device().adapter().properties.deviceName.data(),
                    presenter.queue().device().adapter().driver.driverInfo.data(),presenter.queue().device().adapter().properties.apiVersion);
                run(presenter,window,*heap,vertex,fragment);
            }
        }
        std::printf("frames=144 errors=%u warnings=%u liveAllocations=%zu\n",diagnostics.errors.load(),diagnostics.warnings.load(),heap->snapshot().live_allocations);
        require(diagnostics.errors == 0 && diagnostics.warnings == 0,"validation diagnostics");
        require(heap->snapshot().live_allocations == 0 && system->try_close().closed(),"presentation Memory did not close");
    } catch (const std::exception& error) { std::fprintf(stderr,"%s\n",error.what()); return 1; }
    return 0;
}

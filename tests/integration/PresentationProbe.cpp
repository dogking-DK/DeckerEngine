#include <dk/graphics/Presentation.hpp>
#include <dk/graphics/ShaderCompiler.hpp>
#include <dk/graphics/Transfer.hpp>
#include <dk/memory/MemorySystem.hpp>
#include "PresentationInternal.hpp"
#include "PresentationBridge.hpp"
#include <atomic>
#include <cstdio>
#include <stdexcept>
#include <thread>
#include <cstring>

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
struct Hooks {
    inline static Hooks* active = nullptr;
    Presenter& presenter;
    graphics::detail::PresentationApi original;
    graphics::detail::SubmissionApi submission;
    VkResult acquire_result = VK_SUCCESS, present_result = VK_SUCCESS, wait_result = VK_SUCCESS, submit_result = VK_SUCCESS;
    memory::ResourceHandle close_after_acquire;
    explicit Hooks(Presenter& value) : presenter(value), original(graphics::detail::PresenterAccess::api(value)),
        submission(graphics::detail::PresentationAccess::state(value.queue())->api) {
        require(active == nullptr,"nested presentation hooks"); active = this;
        auto api = original; api.acquire = acquire; api.present = present; api.wait = wait;
        graphics::detail::PresenterAccess::set_api(presenter,api);
        graphics::detail::PresentationAccess::state(presenter.queue())->api.submit = submit;
    }
    ~Hooks() {
        graphics::detail::PresenterAccess::set_api(presenter,original);
        graphics::detail::PresentationAccess::state(presenter.queue())->api = submission;
        active = nullptr;
    }
    static VKAPI_ATTR VkResult VKAPI_CALL acquire(VkDevice device, VkSwapchainKHR chain, std::uint64_t timeout, VkSemaphore signal, VkFence fence, std::uint32_t* index) {
        const auto forced = std::exchange(active->acquire_result,VK_SUCCESS);
        if (forced != VK_SUCCESS && forced != VK_SUBOPTIMAL_KHR) return forced;
        const auto result = active->original.acquire(device,chain,timeout,signal,fence,index);
        if ((result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR) && active->close_after_acquire)
            active->close_after_acquire.begin_close();
        return result == VK_SUCCESS && forced == VK_SUBOPTIMAL_KHR ? forced : result;
    }
    static VKAPI_ATTR VkResult VKAPI_CALL present(VkQueue queue, const VkPresentInfoKHR* info) {
        const auto forced = std::exchange(active->present_result,VK_SUCCESS);
        if (forced == VK_ERROR_OUT_OF_HOST_MEMORY || forced == VK_ERROR_OUT_OF_DEVICE_MEMORY) return forced;
        const auto result = active->original.present(queue,info); // Enqueue actual fence and semaphore wait.
        return (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR) && forced != VK_SUCCESS ? forced : result;
    }
    static VKAPI_ATTR VkResult VKAPI_CALL wait(VkDevice device, std::uint32_t count, const VkFence* fences, VkBool32 all, std::uint64_t timeout) {
        const auto forced = std::exchange(active->wait_result,VK_SUCCESS);
        return forced == VK_SUCCESS ? active->original.wait(device,count,fences,all,timeout) : forced;
    }
    static VKAPI_ATTR VkResult VKAPI_CALL submit(VkQueue queue, std::uint32_t count, const VkSubmitInfo2* infos, VkFence fence) {
        const auto forced = std::exchange(active->submit_result,VK_SUCCESS);
        return forced == VK_SUCCESS ? active->submission.submit(queue,count,infos,fence) : forced;
    }
};
void recovered_frame(Presenter& presenter, const platform::Window& window, const GraphicsPipeline& graphics) {
    auto frame = next_frame(presenter,window);
    draw(frame,graphics);
    take(presenter.present(std::move(frame)));
}
Result<AcquiredFrame> injected_acquire(Presenter& presenter, Hooks& hooks) {
    // A busy frame slot may time out before reaching vkAcquireNextImageKHR.
    // Assert the injected result only after that specific call was reached.
    for (int i = 0; i < 100; ++i) {
        auto result = presenter.acquire();
        if (hooks.acquire_result == VK_SUCCESS || !result) return result;
        require(result->status == AcquireStatus::retry,"frame bypassed pending acquire injection");
    }
    throw std::runtime_error("acquire injection was not reached");
}
void expect_acquire_error(Presenter& presenter) {
    for (int i = 0; i < 100; ++i) {
        auto result = presenter.acquire();
        if (!result) return;
        require(result->status == AcquireStatus::retry,"expected acquisition failure but received a frame");
    }
    throw std::runtime_error("acquisition failure injection was not reached");
}
template<class F> void window_until(const platform::Window& window, F condition) {
    for (int i = 0; i < 100; ++i) {
        if (condition(take(window.poll_events()))) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    throw std::runtime_error("window recovery state did not converge");
}
void recovery(Presenter& presenter, const platform::Window& window, const CompiledShader& vertex, const CompiledShader& fragment) {
    Hooks hooks{presenter};
    auto initial = next_frame(presenter,window);
    auto graphics = pipeline(presenter.queue().resources(),vertex,fragment,initial.format());
    initial = {};
    for (auto result : {VK_TIMEOUT,VK_NOT_READY,VK_ERROR_OUT_OF_HOST_MEMORY}) {
        const auto before = presenter.stats();
        hooks.acquire_result = result;
        auto acquired = injected_acquire(presenter,hooks);
        if (result == VK_ERROR_OUT_OF_HOST_MEMORY) require(!acquired,"acquire error became success");
        else require(acquired && acquired->status == AcquireStatus::retry && !acquired->frame,"acquire timeout/not-ready published frame");
        require(presenter.stats().submitted == before.submitted && presenter.stats().abandoned == before.abandoned,"failed acquire changed frame state");
        recovered_frame(presenter,window,graphics);
    }
    // A frame slot must not be reset/reused until both acquisition and present fences complete.
    hooks.wait_result = VK_TIMEOUT;
    auto delayed = take(presenter.acquire(0));
    require(delayed.status == AcquireStatus::retry && !delayed.frame,"fence timeout reused slot");
    recovered_frame(presenter,window,graphics);
    hooks.wait_result = VK_ERROR_OUT_OF_HOST_MEMORY;
    require(!presenter.acquire(),"fence error was ignored");
    recovered_frame(presenter,window,graphics);

    for (int cycle = 0; cycle < 3; ++cycle) {
        const auto before = presenter.stats().generation;
        take(window.resize(520 + cycle * 40,360 + cycle * 20));
        window_until(window,[&](auto status) { return status.pixel_width == 520u + cycle * 40u && status.pixel_height == 360u + cycle * 20u; });
        auto frame = next_frame(presenter,window);
        require(frame.extent() == vk::Extent2D{520u + cycle * 40u,360u + cycle * 20u},"resize did not update frame extent");
        require(presenter.stats().generation > before,"resize did not rebuild");
        draw(frame,graphics); take(presenter.present(std::move(frame)));
        take(window.minimize());
        window_until(window,[](auto status) { return status.minimized; });
        auto suspended = take(presenter.acquire());
        require(suspended.status == AcquireStatus::suspended && !suspended.frame,"minimized window acquired image");
        take(window.restore());
        window_until(window,[](auto status) { return !status.minimized && status.pixel_width; });
        recovered_frame(presenter,window,graphics);
    }

    hooks.acquire_result = VK_ERROR_OUT_OF_DATE_KHR;
    auto stale = take(injected_acquire(presenter,hooks));
    require(stale.status == AcquireStatus::retry && presenter.stats().needs_rebuild,"out-of-date acquire not scheduled for rebuild");
    recovered_frame(presenter,window,graphics);
    hooks.acquire_result = VK_SUBOPTIMAL_KHR;
    auto suboptimal = next_frame(presenter,window);
    require(presenter.stats().needs_rebuild,"suboptimal acquire did not request rebuild");
    draw(suboptimal,graphics);
    require(take(presenter.present(std::move(suboptimal))).status == PresentStatus::needs_rebuild,"suboptimal frame lost rebuild status");
    recovered_frame(presenter,window,graphics);
    for (auto result : {VK_SUBOPTIMAL_KHR,VK_ERROR_OUT_OF_DATE_KHR}) {
        auto frame = next_frame(presenter,window); draw(frame,graphics);
        hooks.present_result = result;
        require(take(presenter.present(std::move(frame))).status == PresentStatus::needs_rebuild,"present status did not request rebuild");
        recovered_frame(presenter,window,graphics);
    }

    // Keep a view of the retired generation through a failure after native swapchain creation.
    ImageView retired;
    {
        auto frame = next_frame(presenter,window);
        retired = take(presenter.queue().resources().create_view(frame.color()));
    }
    auto prior = presenter.stats().generation;
    take(presenter.request_rebuild());
    graphics::detail::PresenterAccess::fail_after_swapchain(presenter,VK_ERROR_OUT_OF_DEVICE_MEMORY);
    require(!presenter.acquire(),"candidate generation failure was ignored");
    require(presenter.stats().generation == prior && presenter.stats().needs_rebuild,"partial generation was published");
    {
        auto frame = next_frame(presenter,window);
        require(presenter.stats().generation > prior,"failed candidate did not recover");
        require(!frame.commands().retain(retired),"retired generation was usable in new frame");
        draw(frame,graphics); take(presenter.present(std::move(frame)));
    }
    retired = {};
    graphics::detail::ObjectAccess::fail_creation(presenter.queue().resources(),VK_ERROR_OUT_OF_DEVICE_MEMORY);
    const auto abandoned = presenter.stats().abandoned;
    expect_acquire_error(presenter);
    require(presenter.stats().abandoned == abandoned + 1,"failed acquired frame was not released");
    recovered_frame(presenter,window,graphics);

    {
        auto frame = next_frame(presenter,window); draw(frame,graphics);
        auto state = graphics::detail::ObjectAccess::state(frame.color());
        const auto submitted = presenter.stats().submitted;
        hooks.submit_result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
        require(!presenter.present(std::move(frame)) && !frame,"submit failure retained frame");
        require(presenter.stats().submitted == submitted,"failed submit published ticket");
        require(state->states.front().layout == vk::ImageLayout::eUndefined && !state->states.front().initialized,"failed submit published image state");
    }
    recovered_frame(presenter,window,graphics);
    for (auto result : {VK_ERROR_OUT_OF_HOST_MEMORY,VK_ERROR_OUT_OF_DEVICE_MEMORY}) {
        auto frame = next_frame(presenter,window); draw(frame,graphics);
        const auto before = presenter.stats(); hooks.present_result = result;
        require(!presenter.present(std::move(frame)) && !frame,"present OOM was ignored");
        require(presenter.stats().submitted == before.submitted + 1 && presenter.stats().presented == before.presented,
            "present failure rolled back submitted rendering");
        recovered_frame(presenter,window,graphics);
    }
    // Failed drain retains the old generation until completion can be proved.
    prior = presenter.stats().generation;
    take(presenter.request_rebuild()); hooks.wait_result = VK_ERROR_OUT_OF_HOST_MEMORY;
    require(!presenter.acquire() && presenter.stats().generation == prior,"failed drain destroyed old generation");
    recovered_frame(presenter,window,graphics);
    {
        auto frame = next_frame(presenter,window); draw(frame,graphics);
        hooks.present_result = VK_ERROR_SURFACE_LOST_KHR;
        require(!presenter.present(std::move(frame)) && presenter.stats().failed,"surface loss not terminal");
        require(!presenter.acquire(),"lost surface accepted acquisition");
    }
    graphics = {};
    take(presenter.close());
    require(presenter.stats().closed,"failed surface could not close");
    std::printf("recovery generations=%llu submitted=%llu presented=%llu abandoned=%llu\n",
        static_cast<unsigned long long>(presenter.stats().generation),static_cast<unsigned long long>(presenter.stats().submitted),
        static_cast<unsigned long long>(presenter.stats().presented),static_cast<unsigned long long>(presenter.stats().abandoned));
}
void terminal_cases(memory::MemorySystem& system, memory::ResourceHandle resource, const DeviceOptions& options) {
    {
        auto window = take(platform::Window::create(resource,{"DeckerEngine device loss probe",320,240}));
        auto presenter = take(Presenter::create(resource,window,options,{1}));
        Hooks hooks{presenter};
        hooks.acquire_result = VK_ERROR_DEVICE_LOST; // No actual GPU/present work has been enqueued.
        require(!presenter.acquire() && presenter.stats().failed,"device loss not terminal");
        require(!presenter.acquire(),"lost device accepted acquisition");
        require(!presenter.close() && presenter.stats().closed,"lost device close not reported");
        take(presenter.close());
    }
    auto created = system.create_heap({"presentation-oom",memory::DomainCategory::render});
    require(bool(created),"test heap creation failed");
    auto heap = *created;
    {
        auto window = take(platform::Window::create(heap,{"DeckerEngine allocation failure probe",320,240}));
        auto presenter = take(Presenter::create(heap,window,options,{1}));
        Hooks hooks{presenter}; hooks.close_after_acquire = heap;
        bool allocation_failed = false;
        try { expect_acquire_error(presenter); } catch (const std::bad_alloc&) { allocation_failed = true; }
        require(allocation_failed && presenter.stats().abandoned == 1,"post-acquire allocation failure did not release frame");
        require(!presenter.acquire(),"closing heap accepted acquisition");
        take(presenter.close());
    }
    require(heap.snapshot().live_allocations == 0,"post-acquire OOM leaked CPU owners");
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
int main(int argc, char** argv)
{
    const bool test_recovery = argc == 2 && std::strcmp(argv[1],"--recovery") == 0;
    if (argc != 1 && !test_recovery) return 2;
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
            for (int round = 0; round < (test_recovery ? 1 : 3); ++round) {
                auto window = take(platform::Window::create(*heap,{"DeckerEngine presentation probe",480,320}));
                auto created = Presenter::create(*heap,window,options,{test_recovery ? 1u : 2u});
                if (!created) {
                    std::fprintf(stderr,"%s\n",created.error().message.c_str());
                    return created.error().code == ErrorCode::not_supported || created.error().code == ErrorCode::not_found ? 77 : 1;
                }
                auto presenter = std::move(*created);
                std::printf("GPU=%s driver=%s API=%u\n",presenter.queue().device().adapter().properties.deviceName.data(),
                    presenter.queue().device().adapter().driver.driverInfo.data(),presenter.queue().device().adapter().properties.apiVersion);
                if (test_recovery) recovery(presenter,window,vertex,fragment);
                else run(presenter,window,*heap,vertex,fragment);
            }
            if (test_recovery) terminal_cases(*system,*heap,options);
        }
        std::printf("mode=%s errors=%u warnings=%u liveAllocations=%zu\n",test_recovery ? "recovery" : "144 frames",
            diagnostics.errors.load(),diagnostics.warnings.load(),heap->snapshot().live_allocations);
        require(diagnostics.errors == 0 && diagnostics.warnings == 0,"validation diagnostics");
        require(heap->snapshot().live_allocations == 0 && system->try_close().closed(),"presentation Memory did not close");
    } catch (const std::exception& error) { std::fprintf(stderr,"%s\n",error.what()); return 1; }
    return 0;
}

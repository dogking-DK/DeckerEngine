#include <dk/editor/Workbench.hpp>
#include "WorkbenchUi.hpp"
#include "GuiRenderer.hpp"
#include "SmokeDriver.hpp"
#include "ConsistencyDriver.hpp"
#include "ResponseProbe.hpp"
#include "EditorSupport.hpp"
#include <dk/memory/MemorySystem.hpp>
#include <dk/memory/Context.hpp>
#include <dk/graphics/Transfer.hpp>
#include <dk/io/File.hpp>
#include <imgui_impl_sdl3.h>
#include <SDL3/SDL.h>
#include <atomic>
#include <chrono>
#include <cstdio>

namespace dk::editor {
namespace {
using namespace detail;
struct Diagnostics { std::atomic<unsigned> errors=0,warnings=0,known_loader_warnings=0; };
void diagnostic(void* user,const graphics::Diagnostic& message) noexcept {
    auto& d=*static_cast<Diagnostics*>(user);
    if (message.severity==VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) ++d.errors;
    if (message.severity==VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) ++d.warnings;
    if (message.severity==VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT && message.name=="Loader Message" &&
        message.message=="Layer VK_LAYER_AMD_switchable_graphics uses API version 1.3 which is older than the application specified API version of 1.4. May cause issues.")
        ++d.known_loader_warnings;
    if (message.severity>=VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
        std::fprintf(stderr,"%.*s: %.*s\n",static_cast<int>(message.name.size()),message.name.data(),static_cast<int>(message.message.size()),message.message.data());
}
void event_sink(const SDL_Event& event,void* user) {
    ImGui_ImplSDL3_ProcessEvent(&event);
    // Cancel before a minimized loop skips NewFrame; focus may return before rendering resumes.
    if (event.type==SDL_EVENT_WINDOW_FOCUS_LOST || event.type==SDL_EVENT_WINDOW_MINIMIZED)
        static_cast<WorkbenchUi*>(user)->cancel_interaction();
}
void write_capture(const std::filesystem::path& output,const graphics::ReadbackRequest& readback) {
    const auto desc=readback.description();
    const bool bgra=desc.format==vk::Format::eB8G8R8A8Srgb || desc.format==vk::Format::eB8G8R8A8Unorm;
    if (!bgra && desc.format!=vk::Format::eR8G8B8A8Srgb && desc.format!=vk::Format::eR8G8B8A8Unorm)
        throw std::runtime_error("Unsupported workbench capture format");
    std::vector<std::byte> pixels(static_cast<std::size_t>(desc.bytes));
    if (!take(readback.try_read(pixels))) throw std::runtime_error("Workbench readback is not complete");
    const std::string header="P6\n"+std::to_string(desc.width)+" "+std::to_string(desc.height)+"\n255\n";
    const auto bytes=std::as_bytes(std::span{header.data(),header.size()});
    ByteBuffer ppm{bytes.begin(),bytes.end()};
    for (std::uint32_t y=0;y<desc.height;++y) for (std::uint32_t x=0;x<desc.width;++x) {
        const auto offset=static_cast<std::size_t>(y*desc.row_pitch)+x*4;
        ppm.push_back(pixels[offset+(bgra ? 2 : 0)]); ppm.push_back(pixels[offset+1]); ppm.push_back(pixels[offset+(bgra ? 0 : 2)]);
    }
    check(write_file_bytes_atomic(output,ppm));
}
int session(const WorkbenchOptions& options,memory::ResourceHandle heap,Diagnostics& diagnostics) {
    if ((options.smoke || options.consistency_smoke || options.simulation_response_probe) && !std::filesystem::is_regular_file(options.root/".dk-editor-smoke"))
        throw std::runtime_error("--smoke requires a disposable project with .dk-editor-smoke marker");
    ShutdownProbe shutdown{options.simulation_response_probe};
    ShutdownPhase model_phase{shutdown,"model"};
    auto model=take(Workspace::create(options.root));
    check(model->open(options.manifest));
#ifdef _WIN32
    if (!options.pipe.empty()) check(model->start_ipc(options.pipe));
#endif
    ShutdownPhase window_phase{shutdown,"window"};
    auto window=take(platform::Window::create(heap,{"DeckerEngine | Scene Workbench",1440,900}));
    graphics::DeviceOptions device;
    device.validation=options.validation ? graphics::ValidationMode::required : options.disable_validation ?
        graphics::ValidationMode::disabled : graphics::ValidationMode::if_available;
    device.diagnostic_sink=diagnostic; device.diagnostic_user_data=&diagnostics;
#ifdef DK_SIMULATION_GPU
    device.secondary_queue=true;
#endif
    ShutdownPhase presenter_phase{shutdown,"presenter"};
    auto created=graphics::Presenter::create(heap,window,device);
    if (!created) {
        std::fprintf(stderr,"%s\n",created.error().message.c_str());
        return created.error().code==ErrorCode::not_found || created.error().code==ErrorCode::not_supported ? 77 : 1;
    }
    auto presenter=std::move(*created); auto& queue=presenter.queue();
    // This guard is declared after Presenter: every exit joins simulation users
    // before the main-thread view can release its device and window lifetime.
    struct RuntimeBeforeDevice {
        std::unique_ptr<Workspace>& model;
        ~RuntimeBeforeDevice() { model.reset(); }
    } runtime_before_device{model};
#ifdef DK_SIMULATION_GPU
    if (queue.device().queue_count()>1)
        check(model->set_simulation_gpu_device(std::make_shared<graphics::Device>(take(queue.device().share_queue(0)))));
#endif
    if (options.simulation_response_probe) std::printf("simulation_device=%s queues=%u\n",
        queue.device().queue_count()>1 ? "shared" : "independent",queue.device().queue_count());
    if (options.frames || options.smoke || options.consistency_smoke) std::printf("GPU=%s driver=%s\n",queue.device().adapter().properties.deviceName.data(),
        queue.device().adapter().driver.driverInfo.data());
    {
    ShutdownPhase gui_phase{shutdown,"gui"};
    GuiRenderer gui{queue,window};
    ShutdownPhase viewport_phase{shutdown,"viewport"};
    Viewport viewport{heap,queue,options.consistency_smoke};
    WorkbenchUi ui{*model,options.fixture_camera};
    SmokeDriver smoke{options.interaction_smoke};
    std::optional<ConsistencyDriver> consistency;
    std::optional<ResponseProbe> response;
    if (options.simulation_response_probe) response.emplace(options.root);
    if (options.consistency_smoke) consistency.emplace(options.root);
    unsigned frames=0;
    const auto start=std::chrono::steady_clock::now();
    try {
        for (;;) {
            if (response) response->tick(model->simulation_state());
            const auto status=take(window.poll_events(event_sink,&ui));
            if (status.close_requested) { check(window.clear_close_request()); ui.request_close(); }
            model->pump();
            if (ui.closing()) check(model->request_shutdown());
            if (model->stopping()) break;
            if ((options.smoke || options.frames) && std::chrono::steady_clock::now()-start>std::chrono::seconds(90))
                throw std::runtime_error("Workbench acceptance run timed out");
            if (consistency && std::chrono::steady_clock::now()-start>std::chrono::seconds(180))
                throw std::runtime_error("Consistency acceptance run timed out");
            if (status.minimized || !status.pixel_width || !status.pixel_height) { SDL_Delay(16); continue; }
            auto acquired=take(presenter.acquire());
            if (acquired.status!=graphics::AcquireStatus::ready) { SDL_Delay(8); continue; }
            // Acquisition can wait for presentation. Handle requests that arrived
            // during that wait before adding another frame to the presentation queue.
            model->pump();
            if (model->stopping()) break; // Frame returns its unsubmitted acquired image.
            auto& frame=acquired.frame;
            if (!gui.compatible(frame)) {
                viewport.release_texture(); gui.shutdown_gpu(); gui.initialize(frame);
            }
            gui.new_frame([&] {
                if (options.smoke) smoke.input(*model,ui,window,viewport);
                if (consistency) consistency->input(ui);
            });
            const auto format=frame.format();
            const bool srgb=format==vk::Format::eB8G8R8A8Srgb || format==vk::Format::eR8G8B8A8Srgb;
            ui.draw(viewport,srgb);
            ImGui::Render();
            gui.record(frame,viewport.texture() ? &viewport.image() : nullptr,viewport.texture() ? &viewport.image_view() : nullptr);
            ++frames;
            const bool finish=(options.smoke && smoke.done()) || (consistency && consistency->done()) || (options.frames && frames>=options.frames);
            std::optional<graphics::ReadbackRequest> capture;
            if (finish && !options.screenshot.empty()) {
                const auto extent=frame.extent();
                capture=take(frame.commands().readback(frame.color(),{0,0,0,0,extent.width,extent.height}));
            }
            const auto presented=take(presenter.present(std::move(frame)));
            if (!take(queue.wait(presented.completion))) throw std::runtime_error("GUI submission timed out");
            if (consistency) consistency->presented(*model,ui,viewport);
            if (response) response->presented();
            if (capture) write_capture(options.screenshot,*capture);
            if (finish) {
                if (options.smoke && !smoke.done()) throw std::runtime_error("Frame limit interrupted smoke");
                if (!viewport.texture() || !viewport.error().empty() || !viewport.current(model->snapshot()->state))
                    throw std::runtime_error("Viewport did not publish the current scene");
                std::printf("workbench frames=%u revision=%llu draws=%zu viewport=%ux%u smoke=%s\n",frames,
                    static_cast<unsigned long long>(viewport.info().revision),viewport.info().draw_count,
                    viewport.info().width,viewport.info().height,options.smoke ? "passed" : "off");
                break;
            }
            model->pump();
            if (model->stopping()) break;
        }
#ifdef _WIN32
        shutdown.begin();
        model->close_ipc();
        shutdown.mark("ipc_closed");
#endif
        // Rendering completion is sufficient for the viewport/ImGui resources.
        // Keep the swapchain and its presentation fences until after their cleanup.
        check(queue.close());
        shutdown.mark("render_closed");
        if (response) response->finish();
    } catch (...) {
        // Drain before descriptor/backend destruction, including a failed frame.
        const auto closed=presenter.close();
        if (!closed) std::fprintf(stderr,"presenter close: %s\n",closed.error().message.c_str());
        throw;
    }
    }
    // Join the simulation owner before destroying the GUI device. All UI users
    // of the model are gone; outstanding presentation can complete concurrently.
    check(presenter.close());
    shutdown.mark("presenter_closed");
    model.reset();
    shutdown.mark("runtime_closed");
    return 0;
}
}
int run_workbench(const WorkbenchOptions& options) {
    auto system=detail::take(memory::MemorySystem::create());
    auto heap=detail::take(system.create_heap({"editor",memory::DomainCategory::render}));
    Diagnostics diagnostics;
    int result=1;
    {
        memory::ThreadContext context{system,heap}; memory::ExecutionScope scope{context,heap};
        result=session(options,heap,diagnostics);
    }
    const auto live=heap.snapshot().live_allocations;
    const bool closed=system.try_close().closed();
    if (options.simulation_response_probe) std::printf("shutdown phase=memory_closed at_ns=%lld\n",static_cast<long long>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()));
    std::printf("validation errors=%u warnings=%u liveAllocations=%zu knownLoaderWarnings=%u\n",
        diagnostics.errors.load(),diagnostics.warnings.load(),live,diagnostics.known_loader_warnings.load());
    const auto allowed=options.simulation_response_probe ? diagnostics.known_loader_warnings.load() : 0;
    if (result==0 && (diagnostics.errors || diagnostics.warnings!=allowed || live || !closed)) return 1;
    return result;
}
} // namespace dk::editor

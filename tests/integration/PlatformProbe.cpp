#include <dk/platform/Window.hpp>
#include <dk/memory/MemorySystem.hpp>
#include <SDL3/SDL.h>
#include <cstdio>
#include <stdexcept>
#include <thread>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

using namespace dk;
using namespace dk::platform;
namespace {
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
template<class T> T take(Result<T>&& result) {
    if (!result) throw std::runtime_error(result.error().message);
    return std::move(*result);
}
void check(Result<void> result) { if (!result) throw std::runtime_error(result.error().message); }
template<class F> WindowStatus until(const Window& window, F condition) {
    for (int i = 0; i < 100; ++i) {
        auto state = take(window.poll_events());
        if (condition(state)) return state;
        SDL_Delay(10);
    }
    throw std::runtime_error("window state did not converge");
}
#ifdef _WIN32
unsigned foreground_activations = 0;
void CALLBACK observe_foreground(HWINEVENTHOOK, DWORD, HWND hwnd, LONG, LONG, DWORD, DWORD) {
    DWORD process = 0;
    GetWindowThreadProcessId(hwnd, &process);
    if (process == GetCurrentProcessId()) ++foreground_activations;
}
struct ForegroundWatch {
    HWINEVENTHOOK hook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr,
        observe_foreground, 0, 0, WINEVENT_OUTOFCONTEXT);
    ForegroundWatch() { require(hook != nullptr, "foreground event hook unavailable"); }
    ~ForegroundWatch() { UnhookWinEvent(hook); }
};
void check_background(const Window& window) {
    auto* native = take(window.native_sdl_window());
    const auto flags = SDL_GetWindowFlags(native);
    require(!(flags & (SDL_WINDOW_HIDDEN | SDL_WINDOW_MINIMIZED | SDL_WINDOW_NOT_FOCUSABLE | SDL_WINDOW_ALWAYS_ON_TOP)),
        "background window must remain visible and manually focusable");
    auto hwnd = static_cast<HWND>(SDL_GetPointerProperty(SDL_GetWindowProperties(native), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
    require(hwnd && IsWindowVisible(hwnd), "background window is not actually shown");
    require(!(GetWindowLongPtr(hwnd, GWL_EXSTYLE) & (WS_EX_TOPMOST | WS_EX_NOACTIVATE)), "background window lost normal manual activation");
    const auto foreground = GetForegroundWindow();
    require(foreground != hwnd && foreground_activations == 0, "background lifecycle activated a test window");
    if (foreground && foreground != GetDesktopWindow() && foreground != GetShellWindow()) {
        bool behind = false;
        for (auto above = GetTopWindow(nullptr); above && above != hwnd; above = GetWindow(above, GW_HWNDNEXT))
            if (above == foreground) { behind = true; break; }
        require(behind, "background window is above the foreground window");
    }
}
#endif
}
int main()
{
    auto memory = memory::MemorySystem::create();
    if (!memory) return 1;
    auto heap = memory->create_heap({"platform-probe", memory::DomainCategory::render});
    if (!heap) return 1;
    auto resource = *heap;
    try {
        require(!WindowDesc{}.background, "ordinary startup must remain the default");
#ifdef _WIN32
        ForegroundWatch foreground_watch;
#endif
        require(!Window{}.status(), "empty window accepted");
        require(!Window{}.native_sdl_window() && !Window{}.clear_close_request(), "empty window interop accepted");
        require(!Window::create(resource, {"invalid", 0, 100}), "zero width accepted");
        for (int round = 0; round < 3; ++round) {
            WindowDesc description{"DeckerEngine platform probe", 320, 240, true, false, false};
#ifdef _WIN32
            description.background = true;
#endif
            auto created = Window::create(resource, description);
            if (!created) { std::fprintf(stderr, "%s\n", created.error().message.c_str()); return 1; }
            auto window = std::move(*created);
            auto second = take(Window::create(resource, {"Second", 200, 160, true, false, true}));
            until(window, [](auto state) { return state.pixel_width && state.pixel_height; });
#ifdef _WIN32
            check_background(window);
            auto hidden_description = description; hidden_description.hidden = true;
            auto hidden = take(Window::create(resource, hidden_description));
            require(SDL_GetWindowFlags(take(hidden.native_sdl_window())) & SDL_WINDOW_HIDDEN, "background ignored hidden creation");
            require(!(SDL_GetWindowFlags(take(second.native_sdl_window())) & SDL_WINDOW_NOT_FOCUSABLE), "background policy leaked into ordinary window");
#endif
            bool rejected = false;
            std::thread worker([&] { rejected = !window.status() && !window.native_sdl_window() && !window.clear_close_request(); }); worker.join();
            require(rejected, "worker window access accepted");
            require(!window.resize(0, 100), "invalid resize accepted");
            check(window.resize(420, 280));
            until(window, [](auto state) { return state.pixel_width == 420 && state.pixel_height == 280; });
#ifdef _WIN32
            check_background(window);
#endif
            check(window.minimize());
            until(window, [](auto state) { return state.minimized && !state.pixel_width && !state.pixel_height; });
            check(window.restore());
            until(window, [](auto state) { return !state.minimized && state.pixel_width && state.pixel_height; });
#ifdef _WIN32
            check_background(window);
#endif
            SDL_Event close{}; close.type = SDL_EVENT_WINDOW_CLOSE_REQUESTED;
            close.window.windowID = SDL_GetWindowID(take(second.native_sdl_window()));
            require(SDL_PushEvent(&close), "push close event failed");
            require(!take(window.poll_events()).close_requested && take(second.status()).close_requested, "close event routing failed");
            unsigned forwarded=0;
            SDL_Event input{}; input.type=SDL_EVENT_USER;
            require(SDL_PushEvent(&input), "push user event failed");
            take(window.poll_events([](const SDL_Event& event,void* user) {
                if (event.type==SDL_EVENT_USER) ++*static_cast<unsigned*>(user);
            },&forwarded));
            require(forwarded==1, "native event sink lost or duplicated input");
            check(second.clear_close_request());
            require(!take(second.status()).close_requested, "close veto was not applied");
            SDL_Event quit{}; quit.type = SDL_EVENT_QUIT;
            require(SDL_PushEvent(&quit), "push quit event failed");
            require(take(window.poll_events()).close_requested, "quit event not routed");
            auto retained = window;
            window = {};
            require(bool(retained.status()), "shared window lifetime lost");
        }
        require(resource.snapshot().live_allocations == 0, "window CPU owners leaked");
        require(memory->try_close().closed(), "Memory failed to close");
#ifdef _WIN32
        require(foreground_activations == 0, "test window unexpectedly activated");
        std::printf("background: 3 visible lifecycles, focusable, behind foreground, activation_events=%u\n", foreground_activations);
#endif
        std::puts("platform: 3 window lifecycles, resize/minimize/restore, event routing, thread guard; liveAllocations=0");
    } catch (const std::exception& error) { std::fprintf(stderr, "%s\n", error.what()); return 1; }
    return 0;
}

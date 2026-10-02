#include <dk/platform/Window.hpp>
#include <dk/memory/MemorySystem.hpp>
#include "WindowInternal.hpp"
#include <SDL3/SDL.h>
#include <cstdio>
#include <stdexcept>
#include <thread>

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
}
int main()
{
    auto memory = memory::MemorySystem::create();
    if (!memory) return 1;
    auto heap = memory->create_heap({"platform-probe", memory::DomainCategory::render});
    if (!heap) return 1;
    auto resource = *heap;
    try {
        require(!Window{}.status(), "empty window accepted");
        require(!Window::create(resource, {"invalid", 0, 100}), "zero width accepted");
        for (int round = 0; round < 3; ++round) {
            auto created = Window::create(resource, {"DeckerEngine platform probe", 320, 240, true, false, false});
            if (!created) { std::fprintf(stderr, "%s\n", created.error().message.c_str()); return 1; }
            auto window = std::move(*created);
            auto second = take(Window::create(resource, {"Second", 200, 160, true, false, true}));
            until(window, [](auto state) { return state.pixel_width && state.pixel_height; });
            bool rejected = false;
            std::thread worker([&] { rejected = !window.status(); }); worker.join();
            require(rejected, "worker window access accepted");
            require(!window.resize(0, 100), "invalid resize accepted");
            check(window.resize(420, 280));
            until(window, [](auto state) { return state.pixel_width == 420 && state.pixel_height == 280; });
            check(window.minimize());
            until(window, [](auto state) { return state.minimized && !state.pixel_width && !state.pixel_height; });
            check(window.restore());
            until(window, [](auto state) { return !state.minimized && state.pixel_width && state.pixel_height; });
            SDL_Event close{}; close.type = SDL_EVENT_WINDOW_CLOSE_REQUESTED;
            close.window.windowID = SDL_GetWindowID(take(detail::WindowAccess::native(second)));
            require(SDL_PushEvent(&close), "push close event failed");
            require(!take(window.poll_events()).close_requested && take(second.status()).close_requested, "close event routing failed");
            SDL_Event quit{}; quit.type = SDL_EVENT_QUIT;
            require(SDL_PushEvent(&quit), "push quit event failed");
            require(take(window.poll_events()).close_requested, "quit event not routed");
            auto retained = window;
            window = {};
            require(bool(retained.status()), "shared window lifetime lost");
        }
        require(resource.snapshot().live_allocations == 0, "window CPU owners leaked");
        require(memory->try_close().closed(), "Memory failed to close");
        std::puts("platform: 3 window lifecycles, resize/minimize/restore, event routing, thread guard; liveAllocations=0");
    } catch (const std::exception& error) { std::fprintf(stderr, "%s\n", error.what()); return 1; }
    return 0;
}

#include "WindowInternal.hpp"
#include <dk/memory/SmartPtr.hpp>
#include <dk/memory/Containers.hpp>
#define SDL_MAIN_HANDLED
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <algorithm>
#include <limits>
#include <thread>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace dk::platform {
namespace {
Error sdl_error(const char* operation)
{ return {ErrorCode::internal_error, std::string(operation) + ": " + SDL_GetError()}; }
bool valid_size(std::uint32_t width, std::uint32_t height)
{ return width && height && width <= static_cast<std::uint32_t>(std::numeric_limits<int>::max()) && height <= static_cast<std::uint32_t>(std::numeric_limits<int>::max()); }
// Process-wide SDL video/event domain. Entry and final release are main-thread only.
struct Video;
std::weak_ptr<Video> video;
struct Video {
    explicit Video(memory::ResourceHandle resource) : windows(memory::Allocator<SDL_Window*>{resource}) {}
    std::thread::id thread = std::this_thread::get_id();
    Vector<SDL_Window*> windows;
    bool initialized = false;
    ~Video() {
        if (thread != std::this_thread::get_id()) std::terminate();
        if (initialized) SDL_QuitSubSystem(SDL_INIT_VIDEO);
        video.reset();
    }
};
constexpr auto window_key = "dk.window.state";
#ifdef _WIN32
Result<void> background_visibility(SDL_Window* window, UINT command = SW_SHOWNA)
{
    auto hwnd = static_cast<HWND>(SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
    if (!hwnd) return std::unexpected(Error{ErrorCode::internal_error, "SDL window has no Win32 handle"});
    const auto failure = [](const char* operation) -> Result<void> {
        return std::unexpected(Error{ErrorCode::internal_error, std::string(operation) + ": Win32 error " + std::to_string(GetLastError())});
    };
    if (command != SW_SHOWNA) {
        WINDOWPLACEMENT placement{}; placement.length = sizeof(placement);
        if (!GetWindowPlacement(hwnd, &placement)) return failure("GetWindowPlacement");
        placement.showCmd = command;
        if (!SetWindowPlacement(hwnd, &placement)) return failure("SetWindowPlacement");
        if (command == SW_SHOWMINNOACTIVE) return {};
    }
    // Initially hidden creation avoids a focus flash. Native visibility messages update SDL's flags.
    if (!SetWindowPos(hwnd, HWND_BOTTOM, 0, 0, 0, 0, SWP_SHOWWINDOW | SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE))
        return failure("SetWindowPos");
    return {};
}
#endif
}
namespace detail {
struct WindowState {
    explicit WindowState(std::shared_ptr<Video> value) : video(std::move(value)) {}
    std::shared_ptr<Video> video;
    SDL_Window* handle = nullptr;
    bool close_requested = false;
    bool background = false;
    ~WindowState() {
        if (video->thread != std::this_thread::get_id()) std::terminate();
        if (handle) { std::erase(video->windows, handle); SDL_DestroyWindow(handle); }
    }
};
Result<SDL_Window*> WindowAccess::native(const Window& window)
{
    if (!window.state_) return std::unexpected(Error{ErrorCode::invalid_state, "window is empty"});
    if (window.state_->video->thread != std::this_thread::get_id())
        return std::unexpected(Error{ErrorCode::invalid_state, "window operations require the creating main thread"});
    return window.state_->handle;
}
std::shared_ptr<void> WindowAccess::lifetime(const Window& window) { return window.state_; }
}
Result<Window> Window::create(memory::ResourceHandle resource, const WindowDesc& description)
{
    if (!resource || resource.state() != memory::ResourceState::open || !valid_size(description.width, description.height) ||
        description.title.find('\0') != std::string::npos)
        return std::unexpected(Error{ErrorCode::invalid_argument, "window requires open Memory, positive int dimensions and a NUL-free title"});
    if (!SDL_IsMainThread()) return std::unexpected(Error{ErrorCode::invalid_state, "SDL window creation requires the main thread"});
#ifndef _WIN32
    if (description.background) return std::unexpected(Error{ErrorCode::not_supported, "Background windows currently require Windows"});
#endif
    auto session = video.lock();
    if (!session) {
        session = memory::make_shared_in<Video>(resource, resource);
        SDL_SetMainReady();
        if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) return std::unexpected(sdl_error("SDL_InitSubSystem"));
        session->initialized = true;
        video = session;
    }
    auto state = memory::make_shared_in<detail::WindowState>(resource, session);
    state->background = description.background;
    session->windows.reserve(session->windows.size() + 1);
    SDL_WindowFlags flags = SDL_WINDOW_HIGH_PIXEL_DENSITY;
    if (description.resizable) flags |= SDL_WINDOW_RESIZABLE;
    if (description.vulkan) flags |= SDL_WINDOW_VULKAN;
    if (description.hidden || description.background) flags |= SDL_WINDOW_HIDDEN;
    state->handle = SDL_CreateWindow(description.title.c_str(), static_cast<int>(description.width), static_cast<int>(description.height), flags);
    if (!state->handle) return std::unexpected(sdl_error("SDL_CreateWindow"));
    if (!SDL_SetPointerProperty(SDL_GetWindowProperties(state->handle), window_key, state.get()))
        return std::unexpected(sdl_error("SDL_SetPointerProperty"));
#ifdef _WIN32
    if (description.background && !description.hidden) {
        auto shown = background_visibility(state->handle);
        if (!shown) return std::unexpected(shown.error());
    }
#endif
    session->windows.push_back(state->handle);
    return Window{std::move(state)};
}
Result<WindowStatus> Window::status() const
{
    auto native = detail::WindowAccess::native(*this);
    if (!native) return std::unexpected(native.error());
    int width = 0, height = 0;
    if (!SDL_GetWindowSizeInPixels(*native, &width, &height)) return std::unexpected(sdl_error("SDL_GetWindowSizeInPixels"));
    const bool minimized = (SDL_GetWindowFlags(*native) & SDL_WINDOW_MINIMIZED) != 0;
    return WindowStatus{minimized ? 0u : static_cast<std::uint32_t>(std::max(0, width)),
        minimized ? 0u : static_cast<std::uint32_t>(std::max(0, height)), minimized, state_->close_requested};
}
Result<SDL_Window*> Window::native_sdl_window() const { return detail::WindowAccess::native(*this); }
Result<void> Window::clear_close_request() const
{
    if (auto native = detail::WindowAccess::native(*this); !native) return std::unexpected(native.error());
    state_->close_requested = false;
    return {};
}
Result<WindowStatus> Window::poll_events(EventSink sink, void* user) const
{
    if (auto native = detail::WindowAccess::native(*this); !native) return std::unexpected(native.error());
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (sink) sink(event, user);
        if (event.type == SDL_EVENT_QUIT) {
            for (auto* window : state_->video->windows)
                static_cast<detail::WindowState*>(SDL_GetPointerProperty(SDL_GetWindowProperties(window), window_key, nullptr))->close_requested = true;
        } else if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
            if (auto* window = SDL_GetWindowFromID(event.window.windowID))
                if (auto* target = static_cast<detail::WindowState*>(SDL_GetPointerProperty(SDL_GetWindowProperties(window), window_key, nullptr)))
                    target->close_requested = true;
        }
    }
    return status();
}
Result<void> Window::resize(std::uint32_t width, std::uint32_t height) const
{
    auto native = detail::WindowAccess::native(*this);
    if (!native) return std::unexpected(native.error());
    if (!valid_size(width, height)) return std::unexpected(Error{ErrorCode::invalid_argument, "window size must be positive int dimensions"});
    if (!SDL_SetWindowSize(*native, static_cast<int>(width), static_cast<int>(height))) return std::unexpected(sdl_error("SDL_SetWindowSize"));
    return {};
}
Result<void> Window::minimize() const
{
    auto native = detail::WindowAccess::native(*this);
    if (!native) return std::unexpected(native.error());
#ifdef _WIN32
    if (state_->background) return background_visibility(*native, SW_SHOWMINNOACTIVE);
#endif
    if (!SDL_MinimizeWindow(*native)) return std::unexpected(sdl_error("SDL_MinimizeWindow"));
    return {};
}
Result<void> Window::restore() const
{
    auto native = detail::WindowAccess::native(*this);
    if (!native) return std::unexpected(native.error());
#ifdef _WIN32
    if (state_->background) return background_visibility(*native, SW_SHOWNOACTIVATE);
#endif
    if (!SDL_RestoreWindow(*native)) return std::unexpected(sdl_error("SDL_RestoreWindow"));
    return {};
}
} // namespace dk::platform

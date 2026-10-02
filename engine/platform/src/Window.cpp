#include "WindowInternal.hpp"
#include <dk/memory/SmartPtr.hpp>
#include <dk/memory/Containers.hpp>
#define SDL_MAIN_HANDLED
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <algorithm>
#include <limits>
#include <thread>

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
}
namespace detail {
struct WindowState {
    explicit WindowState(std::shared_ptr<Video> value) : video(std::move(value)) {}
    std::shared_ptr<Video> video;
    SDL_Window* handle = nullptr;
    bool close_requested = false;
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
    auto session = video.lock();
    if (!session) {
        session = memory::make_shared_in<Video>(resource, resource);
        SDL_SetMainReady();
        if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) return std::unexpected(sdl_error("SDL_InitSubSystem"));
        session->initialized = true;
        video = session;
    }
    auto state = memory::make_shared_in<detail::WindowState>(resource, session);
    session->windows.reserve(session->windows.size() + 1);
    SDL_WindowFlags flags = SDL_WINDOW_HIGH_PIXEL_DENSITY;
    if (description.resizable) flags |= SDL_WINDOW_RESIZABLE;
    if (description.vulkan) flags |= SDL_WINDOW_VULKAN;
    if (description.hidden) flags |= SDL_WINDOW_HIDDEN;
    state->handle = SDL_CreateWindow(description.title.c_str(), static_cast<int>(description.width), static_cast<int>(description.height), flags);
    if (!state->handle) return std::unexpected(sdl_error("SDL_CreateWindow"));
    if (!SDL_SetPointerProperty(SDL_GetWindowProperties(state->handle), window_key, state.get()))
        return std::unexpected(sdl_error("SDL_SetPointerProperty"));
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
Result<WindowStatus> Window::poll_events() const
{
    if (auto native = detail::WindowAccess::native(*this); !native) return std::unexpected(native.error());
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
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
    if (!SDL_MinimizeWindow(*native)) return std::unexpected(sdl_error("SDL_MinimizeWindow"));
    return {};
}
Result<void> Window::restore() const
{
    auto native = detail::WindowAccess::native(*this);
    if (!native) return std::unexpected(native.error());
    if (!SDL_RestoreWindow(*native)) return std::unexpected(sdl_error("SDL_RestoreWindow"));
    return {};
}
} // namespace dk::platform

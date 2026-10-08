#pragma once
#include <dk/core/Result.hpp>
#include <dk/memory/Resource.hpp>
#include <memory>
#include <string>

struct SDL_Window;
union SDL_Event;
namespace dk::platform {
namespace detail { struct WindowState; struct WindowAccess; }
struct WindowDesc {
    std::string title = "DeckerEngine";
    std::uint32_t width = 960, height = 640;
    bool resizable = true, vulkan = true, hidden = false;
};
struct WindowStatus {
    std::uint32_t pixel_width = 0, pixel_height = 0;
    bool minimized = false, close_requested = false;
};
// Shared window lifetime. All calls and final release belong to the main thread.
class Window final {
public:
    Window() = default;
    [[nodiscard]] static Result<Window> create(memory::ResourceHandle resource, const WindowDesc& description = {});
    [[nodiscard]] explicit operator bool() const noexcept { return bool(state_); }
    // Pumps events for all dk windows; returns this window's actual pixel size.
    using EventSink = void (*)(const SDL_Event&, void*);
    // Synchronous borrowed events; sink must not throw or recursively pump events.
    [[nodiscard]] Result<WindowStatus> poll_events(EventSink sink = nullptr, void* user = nullptr) const;
    // Explicit SDL interop. Borrowed, main-thread only; never destroy this handle.
    [[nodiscard]] Result<SDL_Window*> native_sdl_window() const;
    [[nodiscard]] Result<void> clear_close_request() const;
    [[nodiscard]] Result<WindowStatus> status() const;
    [[nodiscard]] Result<void> resize(std::uint32_t width, std::uint32_t height) const;
    [[nodiscard]] Result<void> minimize() const;
    [[nodiscard]] Result<void> restore() const;
private:
    friend struct detail::WindowAccess;
    explicit Window(std::shared_ptr<detail::WindowState> state) : state_(std::move(state)) {}
    std::shared_ptr<detail::WindowState> state_;
};
} // namespace dk::platform

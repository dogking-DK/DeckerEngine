#pragma once
#include <dk/platform/Window.hpp>
struct SDL_Window;
namespace dk::platform::detail {
struct WindowAccess {
    static Result<SDL_Window*> native(const Window& window);
    static std::shared_ptr<void> lifetime(const Window& window);
};
} // namespace dk::platform::detail

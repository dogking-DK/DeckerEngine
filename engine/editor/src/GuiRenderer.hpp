#pragma once
#include <dk/graphics/Presentation.hpp>
#include <imgui.h>
#include <functional>

namespace dk::editor::detail {
// Narrow third-party Vulkan interop. All application resources still use the queue ledger.
class GuiRenderer final {
public:
    GuiRenderer(graphics::SubmissionQueue&,platform::Window&);
    ~GuiRenderer();
    void initialize(const graphics::Frame&);
    [[nodiscard]] bool compatible(const graphics::Frame&) const;
    void shutdown_gpu();
    void new_frame(const std::function<void()>& input = {});
    void record(graphics::Frame&,const graphics::Image* texture,const graphics::ImageView* view);
private:
    graphics::SubmissionQueue& queue_;
    ImGuiStyle style_srgb_;
    bool platform_ = false, gpu_ = false;
    vk::Format format_ = vk::Format::eUndefined;
    std::uint32_t images_ = 0;
};
}

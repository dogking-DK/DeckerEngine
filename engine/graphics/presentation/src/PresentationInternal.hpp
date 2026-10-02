#pragma once
#include <dk/graphics/Presentation.hpp>
namespace dk::graphics::detail {
struct PresentationApi {
    PFN_vkCreateSwapchainKHR create = nullptr;
    PFN_vkAcquireNextImageKHR acquire = nullptr;
    PFN_vkQueuePresentKHR present = nullptr;
    PFN_vkReleaseSwapchainImagesEXT release = nullptr;
    PFN_vkWaitForFences wait = nullptr;
};
// Private deterministic fault seam. Production always uses the device dispatcher.
struct PresenterAccess {
    static PresentationApi api(const Presenter& presenter);
    static void set_api(Presenter& presenter, PresentationApi api);
    static void fail_after_swapchain(Presenter& presenter, VkResult result);
};
} // namespace dk::graphics::detail

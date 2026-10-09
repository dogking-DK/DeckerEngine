#pragma once
#include <dk/graphics/Resources.hpp>

namespace dk::graphics::detail {
// Private driver-failure seam. Null fields use the device's own dispatcher.
struct SubmissionApi {
    PFN_vkQueueSubmit2 submit = nullptr;
    PFN_vkGetSemaphoreCounterValue counter = nullptr;
    PFN_vkWaitSemaphores wait = nullptr;
    PFN_vkGetQueryPoolResults query = nullptr;
};
struct SubmissionAccess {
    static Result<SubmissionQueue> create(memory::ResourceHandle resource, Device&& device,
        std::uint32_t slots, SubmissionApi api = {});
};
} // namespace dk::graphics::detail

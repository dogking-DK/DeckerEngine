#pragma once
#include <dk/graphics/Device.hpp>

namespace dk::graphics::detail {
// Private test seam. A supplied resolver remains alive through Device lifetime.
struct DeviceAccess {
    static Result<Device> create(memory::ResourceHandle resource, const DeviceOptions& options,
                                 PFN_vkGetInstanceProcAddr resolver);
};
} // namespace dk::graphics::detail

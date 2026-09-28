#pragma once
#include <dk/graphics/Device.hpp>

namespace dk::graphics::detail {
// Private test seam. A supplied resolver remains alive through Device lifetime.
struct DeviceAccess {
    struct AllocatorApi {
        decltype(&vmaCreateAllocator) create = vmaCreateAllocator;
        decltype(&vmaDestroyAllocator) destroy = vmaDestroyAllocator;
    };
    static Result<Device> create(memory::ResourceHandle resource, const DeviceOptions& options,
                                 PFN_vkGetInstanceProcAddr resolver, const AllocatorApi* allocator_api = nullptr);
};
} // namespace dk::graphics::detail

#pragma once
#include "ObjectInternal.hpp"

namespace dk::graphics::detail {
struct AccelerationState : ObjectState {
    AccelerationState(std::shared_ptr<DeviceLifetime> device, memory::ResourceHandle resource, AccelerationStructureKind value)
        : ObjectState(std::move(device)), kind(value), inputs(memory::Allocator<Buffer>{resource}),
          children(memory::Allocator<std::shared_ptr<AccelerationState>>{resource}),
          geometries(memory::Allocator<vk::AccelerationStructureGeometryKHR>{resource}),
          ranges(memory::Allocator<vk::AccelerationStructureBuildRangeInfoKHR>{resource}) {}
    AccelerationStructureKind kind;
    AccelerationStructureSizes sizes;
    AccelerationStructureState state;
    bool reserved = false;
    Buffer storage;
    Vector<Buffer> inputs;
    Vector<std::shared_ptr<AccelerationState>> children; // TLAS keeps every referenced BLAS alive.
    Vector<vk::AccelerationStructureGeometryKHR> geometries;
    Vector<vk::AccelerationStructureBuildRangeInfoKHR> ranges;
    vk::DeviceAddress address = 0;
    // Declared last: destroyed before storage, children and Device.
    vk::raii::AccelerationStructureKHR acceleration{nullptr};
    vk::AccelerationStructureBuildGeometryInfoKHR build_info() const {
        vk::AccelerationStructureBuildGeometryInfoKHR info{};
        info.setType(kind == AccelerationStructureKind::bottom_level ? vk::AccelerationStructureTypeKHR::eBottomLevel : vk::AccelerationStructureTypeKHR::eTopLevel)
            .setFlags(vk::BuildAccelerationStructureFlagBitsKHR::ePreferFastTrace).setMode(vk::BuildAccelerationStructureModeKHR::eBuild)
            .setGeometries(geometries);
        return info;
    }
};
} // namespace dk::graphics::detail

#pragma once
#include <dk/graphics/AccelerationStructure.hpp>
#include "ResourcePolicy.hpp"
#include <cmath>

namespace dk::graphics::detail {
inline constexpr auto acceleration_input_usage = vk::BufferUsageFlagBits::eAccelerationStructureBuildInputReadOnlyKHR |
    vk::BufferUsageFlagBits::eShaderDeviceAddress;
inline Result<void> acceleration_count(std::uint64_t count, std::uint64_t limit)
{
    if (!count || count > limit || count > UINT32_MAX)
        return std::unexpected(Error{ErrorCode::invalid_argument, "AS count is empty or exceeds device/32-bit limits"});
    return {};
}
inline Result<vk::DeviceAddress> acceleration_address(vk::DeviceAddress address, vk::DeviceSize offset,
    vk::DeviceSize bytes, vk::DeviceSize alignment)
{
    constexpr auto max = std::numeric_limits<vk::DeviceAddress>::max();
    if (!address || !alignment || offset > max-address || (address+offset)%alignment || !bytes || bytes-1 > max-(address+offset))
        return std::unexpected(Error{ErrorCode::invalid_argument, "AS address is zero, misaligned or overflows"});
    return address+offset;
}
inline Result<vk::DeviceSize> scratch_capacity(vk::DeviceSize bytes, vk::DeviceSize alignment)
{
    if (!alignment || !std::has_single_bit(alignment) || bytes > std::numeric_limits<vk::DeviceSize>::max()-(alignment-1))
        return std::unexpected(Error{ErrorCode::invalid_argument, "invalid AS scratch alignment or size overflow"});
    return bytes ? bytes+alignment-1 : 0;
}
inline Result<vk::DeviceAddress> scratch_address(vk::DeviceAddress address, vk::DeviceSize capacity,
    vk::DeviceSize bytes, vk::DeviceSize alignment)
{
    if (auto result = scratch_capacity(bytes, alignment); !result) return std::unexpected(result.error());
    const auto offset = (alignment-address%alignment)%alignment;
    if (!valid_range(capacity, offset, bytes))
        return std::unexpected(Error{ErrorCode::invalid_argument, "AS scratch does not fit aligned range"});
    return acceleration_address(address, offset, bytes, alignment);
}
inline Result<void> triangle_description(const TriangleGeometry& geometry, const BufferDesc& vertices,
    const BufferDesc* indices, std::uint64_t primitive_limit)
{
    if (geometry.vertex_format != vk::Format::eR32G32B32Sfloat)
        return std::unexpected(Error{ErrorCode::not_supported, "AS triangles require R32G32B32Sfloat vertices"});
    if (auto count = acceleration_count(geometry.primitive_count, primitive_limit); !count) return count;
    if (!geometry.vertex_count || geometry.vertex_stride < 12 || geometry.vertex_stride > UINT32_MAX || geometry.vertex_stride%4 || geometry.vertex_offset%4 ||
        (vertices.usage & acceleration_input_usage) != acceleration_input_usage)
        return std::unexpected(Error{ErrorCode::invalid_argument, "invalid AS vertex count, stride, offset or input usage"});
    const auto last = vk::DeviceSize{geometry.vertex_count-1}*geometry.vertex_stride;
    if (last > std::numeric_limits<vk::DeviceSize>::max()-12 || !valid_range(vertices.size, geometry.vertex_offset, last+12))
        return std::unexpected(Error{ErrorCode::invalid_argument, "AS vertex range exceeds buffer"});
    if (indices) {
        const auto width = geometry.index_type == vk::IndexType::eUint16 ? 2u : geometry.index_type == vk::IndexType::eUint32 ? 4u : 0u;
        if (!width || geometry.index_offset%width || (indices->usage & acceleration_input_usage) != acceleration_input_usage ||
            !valid_range(indices->size, geometry.index_offset, vk::DeviceSize{geometry.primitive_count}*3*width))
            return std::unexpected(Error{ErrorCode::invalid_argument, "invalid AS index type, offset, range or input usage"});
    } else if (geometry.index_offset || vk::DeviceSize{geometry.primitive_count}*3 > geometry.vertex_count) {
        return std::unexpected(Error{ErrorCode::invalid_argument, "non-indexed AS triangle range exceeds vertices"});
    }
    return {};
}
inline Result<void> instance_description(const AccelerationStructureInstance& instance)
{
    constexpr auto allowed = vk::GeometryInstanceFlagBitsKHR::eTriangleFacingCullDisable | vk::GeometryInstanceFlagBitsKHR::eTriangleFlipFacing |
        vk::GeometryInstanceFlagBitsKHR::eForceOpaque | vk::GeometryInstanceFlagBitsKHR::eForceNoOpaque;
    if (instance.custom_index > 0xffffffu || instance.sbt_record_offset > 0xffffffu || instance.mask > 0xffu ||
        (static_cast<VkGeometryInstanceFlagsKHR>(instance.flags) & ~static_cast<VkGeometryInstanceFlagsKHR>(allowed)) || ((instance.flags & vk::GeometryInstanceFlagBitsKHR::eForceOpaque) &&
        (instance.flags & vk::GeometryInstanceFlagBitsKHR::eForceNoOpaque)))
        return std::unexpected(Error{ErrorCode::invalid_argument, "AS instance fields or flags are out of range"});
    for (const auto value : instance.transform) if (!std::isfinite(value))
        return std::unexpected(Error{ErrorCode::invalid_argument, "AS instance transform must be finite"});
    const auto& m = instance.transform;
    const double determinant = double(m[0])*(double(m[5])*m[10]-double(m[6])*m[9]) -
        double(m[1])*(double(m[4])*m[10]-double(m[6])*m[8]) + double(m[2])*(double(m[4])*m[9]-double(m[5])*m[8]);
    if (!std::isfinite(determinant) || determinant == 0)
        return std::unexpected(Error{ErrorCode::invalid_argument, "AS instance transform must be invertible"});
    return {};
}
} // namespace dk::graphics::detail

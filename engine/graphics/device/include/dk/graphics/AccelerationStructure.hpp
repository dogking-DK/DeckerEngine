#pragma once
#include <dk/graphics/GpuObjects.hpp>

namespace dk::graphics {
namespace detail { struct AccelerationState; }
enum class AccelerationStructureKind { bottom_level, top_level };
enum class AccelerationStructureStatus { candidate, submitted, completed, device_lost };
struct AccelerationStructureSizes {
    vk::DeviceSize storage = 0, scratch = 0;
    vk::DeviceSize scratch_alignment = 0;
};
struct AccelerationStructureState {
    AccelerationStructureStatus status = AccelerationStructureStatus::candidate;
    AccessState access;
    // First successful BUILD submission's timeline value, local to its submission domain.
    std::uint64_t generation = 0;
};
// Queue-domain object. Serialize access with its queue; native handles/addresses are borrowed.
// Creation owns a candidate; only successful submit publishes a generation, collection completes it.
class AccelerationStructure final {
public:
    AccelerationStructure() = default;
    AccelerationStructure(AccelerationStructure&&) noexcept = default;
    AccelerationStructure& operator=(AccelerationStructure&&) noexcept = default;
    AccelerationStructure(const AccelerationStructure&) = delete;
    AccelerationStructure& operator=(const AccelerationStructure&) = delete;
    [[nodiscard]] explicit operator bool() const noexcept { return bool(state_); }
    [[nodiscard]] AccelerationStructure share() const noexcept { return AccelerationStructure{state_}; }
    [[nodiscard]] vk::AccelerationStructureKHR handle() const noexcept;
    [[nodiscard]] AccelerationStructureKind kind() const noexcept;
    [[nodiscard]] AccelerationStructureSizes sizes() const noexcept;
    // Candidate addresses are valid for forming instances, not proof of built content.
    [[nodiscard]] Result<vk::DeviceAddress> device_address() const;
    [[nodiscard]] Result<AccelerationStructureState> state() const;
private:
    friend class ResourceFactory;
    friend class CommandBatch;
    friend struct detail::ObjectAccess;
    explicit AccelerationStructure(std::shared_ptr<detail::AccelerationState> state) : state_(std::move(state)) {}
    std::shared_ptr<detail::AccelerationState> state_;
};
struct TriangleGeometry {
    const Buffer* vertices = nullptr;
    vk::DeviceSize vertex_offset = 0, vertex_stride = 12;
    std::uint32_t vertex_count = 0, primitive_count = 0;
    vk::Format vertex_format = vk::Format::eR32G32B32Sfloat;
    const Buffer* indices = nullptr; // Null selects non-indexed triangles.
    vk::DeviceSize index_offset = 0;
    vk::IndexType index_type = vk::IndexType::eUint32;
    bool opaque = true;
    // All indices must be < vertex_count. GPU input contents are caller-owned.
};
struct AccelerationStructureInstance {
    const AccelerationStructure* blas = nullptr;
    std::array<float, 12> transform{1,0,0,0, 0,1,0,0, 0,0,1,0}; // Row-major affine 3x4.
    std::uint32_t custom_index = 0, sbt_record_offset = 0, mask = 0xff;
    vk::GeometryInstanceFlagsKHR flags{};
};
} // namespace dk::graphics

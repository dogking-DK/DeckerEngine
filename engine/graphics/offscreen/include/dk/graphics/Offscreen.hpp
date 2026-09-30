#pragma once
#include <dk/graphics/Resources.hpp>
#include <dk/graphics/ShaderCompiler.hpp>

namespace dk::graphics {
inline constexpr std::uint64_t offscreen_wait_timeout_ns = 10'000'000'000ULL;
struct OffscreenDraw {
    std::uint32_t width = 64, height = 64, vertex_count = 3;
    std::array<float, 4> clear_color{0, 0, 0, 1};
};
struct OffscreenImage {
    std::uint32_t width = 0, height = 0;
    Vector<std::byte> rgba8; // Tightly packed, positive-height Vulkan viewport row order.
};
struct ComputeBufferInput {
    std::uint32_t binding = 0; // Descriptor set 0; one storage buffer per binding.
    std::span<const std::byte> bytes; // Copied into device memory; caller's bytes never modified.
};
struct ComputeBufferOutput { std::uint32_t binding = 0; Vector<std::byte> bytes; };
using ComputeOutput = Vector<ComputeBufferOutput>;
namespace detail { struct OffscreenAccess; }

// Synchronous validation baseline, externally serialized. No pipeline cache or window.
// Consume unmodified, trusted compile_shader artifacts; not a general SPIR-V validator.
// Memory resource must stay open throughout draw/dispatch; drain also works while closing.
class OffscreenExecutor final {
public:
    [[nodiscard]] static Result<OffscreenExecutor> create(memory::ResourceHandle resource, Device&& device);
    ~OffscreenExecutor(); // Drains the queue before releasing pending Vulkan objects.
    OffscreenExecutor(OffscreenExecutor&&) noexcept;
    OffscreenExecutor& operator=(OffscreenExecutor&&) noexcept;
    OffscreenExecutor(const OffscreenExecutor&) = delete;
    OffscreenExecutor& operator=(const OffscreenExecutor&) = delete;
    [[nodiscard]] Result<OffscreenImage> draw(const CompiledShader& vertex, const CompiledShader& fragment,
        const OffscreenDraw& description = {}, std::uint64_t timeout_ns = offscreen_wait_timeout_ns);
    [[nodiscard]] Result<ComputeOutput> dispatch(const CompiledShader& shader, std::span<const ComputeBufferInput> buffers,
        std::span<const std::byte> push_constants, std::array<std::uint32_t, 3> groups,
        std::uint64_t timeout_ns = offscreen_wait_timeout_ns);
    // After timeout/wait failure, new operations are rejected until drain succeeds.
    // Discards that failed call's result. False means still pending; errors retain work.
    [[nodiscard]] Result<bool> drain(std::uint64_t timeout_ns = offscreen_wait_timeout_ns);
    // Accessors require a non-moved-from executor; Device is borrowed.
    [[nodiscard]] const Device& device() const noexcept;
    [[nodiscard]] SubmissionStats stats() const noexcept;
private:
    friend struct detail::OffscreenAccess;
    struct Impl;
    explicit OffscreenExecutor(memory::UniquePtr<Impl> impl);
    memory::UniquePtr<Impl> impl_;
};
} // namespace dk::graphics

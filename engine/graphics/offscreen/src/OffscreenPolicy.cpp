#include "OffscreenPolicy.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace dk::graphics::detail {
namespace {
Error invalid(std::string message) { return {ErrorCode::invalid_argument, std::move(message)}; }
Error unsupported(std::string message) { return {ErrorCode::not_supported, std::move(message)}; }
}
Result<void> validate_shader(const CompiledShader& shader, ShaderStage stage)
{
    if (shader.stage != stage || shader.entry.empty() || shader.entry.find('\0') != String::npos)
        return std::unexpected(invalid("shader entry/stage does not match the operation"));
    const auto& words = shader.spirv;
    if (words.size() < 5 || words[0] != 0x07230203U || words[1] != 0x00010500U || !words[3] || words[4])
        return std::unexpected(invalid("expected a Slang SPIR-V 1.5 artifact"));
    const std::uint32_t model = stage == ShaderStage::vertex ? 0U : stage == ShaderStage::fragment ? 4U : 5U;
    unsigned entries = 0;
    for (std::size_t i = 5; i < words.size();) {
        const auto count = words[i] >> 16;
        const auto opcode = words[i] & 0xFFFFU;
        if (!count || count > words.size() - i) return std::unexpected(invalid("malformed SPIR-V instruction length"));
        if (opcode == 17) { // OpCapability: no optional features enabled on the device.
            if (count != 2) return std::unexpected(invalid("malformed SPIR-V capability"));
            if (words[i + 1] != 0 && words[i + 1] != 1)
                return std::unexpected(unsupported("shader requires a capability outside the baseline Shader/Matrix features"));
        }
        if (opcode == 15) { // OpEntryPoint.
            if (count < 4 || words[i + 1] != model) return std::unexpected(invalid("SPIR-V entry execution model mismatch"));
            const auto* name = reinterpret_cast<const char*>(&words[i + 3]);
            const auto* end = static_cast<const char*>(std::memchr(name, 0, (count - 3) * sizeof(std::uint32_t)));
            if (!end || std::string_view{name, static_cast<std::size_t>(end - name)} != std::string_view{shader.entry})
                return std::unexpected(invalid("SPIR-V entry name mismatch"));
            ++entries;
        }
        i += count;
    }
    if (entries != 1) return std::unexpected(invalid("exactly one SPIR-V entry point is required"));
    return {};
}
Result<std::size_t> validate_draw(const CompiledShader& vertex, const CompiledShader& fragment,
    const OffscreenDraw& desc, const vk::PhysicalDeviceLimits& limits)
{
    if (auto result = validate_shader(vertex, ShaderStage::vertex); !result) return std::unexpected(result.error());
    if (auto result = validate_shader(fragment, ShaderStage::fragment); !result) return std::unexpected(result.error());
    if (!vertex.bindings.empty() || !fragment.bindings.empty() || !vertex.push_constants.empty() || !fragment.push_constants.empty())
        return std::unexpected(unsupported("offscreen draw requires shaders without descriptors or push constants"));
    if (!desc.width || !desc.height || !desc.vertex_count || desc.vertex_count % 3 != 0)
        return std::unexpected(invalid("draw requires a nonempty image and complete triangle list"));
    for (float value : desc.clear_color) if (!std::isfinite(value) || value < 0 || value > 1)
        return std::unexpected(invalid("RGBA clear components must be finite and in [0,1]"));
    if (desc.width > limits.maxImageDimension2D || desc.height > limits.maxImageDimension2D
        || desc.width > limits.maxFramebufferWidth || desc.height > limits.maxFramebufferHeight
        || desc.width > limits.maxViewportDimensions[0] || desc.height > limits.maxViewportDimensions[1]
        || static_cast<float>(desc.width) > limits.viewportBoundsRange[1]
        || static_cast<float>(desc.height) > limits.viewportBoundsRange[1])
        return std::unexpected(unsupported("offscreen extent exceeds device image/framebuffer/viewport limits"));
    const auto pixels = std::uint64_t{desc.width} * desc.height;
    if (pixels > std::numeric_limits<std::size_t>::max() / 4)
        return std::unexpected(invalid("offscreen readback byte size overflows"));
    return static_cast<std::size_t>(pixels * 4);
}
Result<void> validate_dispatch(const CompiledShader& shader, std::span<const ComputeBufferInput> buffers,
    std::span<const std::byte> push, std::array<std::uint32_t, 3> groups, const vk::PhysicalDeviceLimits& limits)
{
    if (auto result = validate_shader(shader, ShaderStage::compute); !result) return result;
    std::uint32_t invocations = 1;
    for (std::size_t i = 0; i < 3; ++i) {
        const auto local = shader.thread_group_size[i];
        if (!groups[i] || !local) return std::unexpected(invalid("dispatch and local dimensions must be nonzero"));
        if (groups[i] > limits.maxComputeWorkGroupCount[i] || local > limits.maxComputeWorkGroupSize[i]
            || local > limits.maxComputeWorkGroupInvocations / invocations)
            return std::unexpected(unsupported("compute workgroup count/size exceeds device limits"));
        invocations *= local;
    }
    if (buffers.empty() || buffers.size() != shader.bindings.size())
        return std::unexpected(invalid("supply every reflected storage buffer exactly once"));
    const auto maximum = std::min({limits.maxDescriptorSetStorageBuffers, limits.maxPerStageDescriptorStorageBuffers, limits.maxPerStageResources});
    if (buffers.size() > maximum || limits.maxBoundDescriptorSets == 0)
        return std::unexpected(unsupported("storage descriptor count exceeds device limits"));
    for (std::size_t i = 0; i < buffers.size(); ++i) {
        const auto& input = buffers[i];
        if (input.bytes.empty() || input.bytes.size() % 4 != 0)
            return std::unexpected(invalid("storage buffers require nonempty four-byte-aligned data"));
        if (input.bytes.size() > limits.maxStorageBufferRange)
            return std::unexpected(unsupported("storage buffer range exceeds device limits"));
        for (std::size_t j = 0; j < i; ++j) if (buffers[j].binding == input.binding)
            return std::unexpected(invalid("duplicate storage buffer input binding"));
    }
    for (std::size_t i = 0; i < shader.bindings.size(); ++i) {
        const auto& binding = shader.bindings[i];
        if (binding.set || binding.count != 1 || binding.type != ShaderDescriptorType::storage_buffer)
            return std::unexpected(unsupported("dispatch supports only set 0 scalar storage buffer descriptors"));
        for (std::size_t j = 0; j < i; ++j) if (shader.bindings[j].binding == binding.binding)
            return std::unexpected(invalid("duplicate reflected storage binding"));
        if (std::none_of(buffers.begin(), buffers.end(), [&](const ComputeBufferInput& input) { return input.binding == binding.binding; }))
            return std::unexpected(invalid("storage input does not match reflection"));
    }
    if (shader.push_constants.size() > 1) return std::unexpected(unsupported("only one push constant range is supported"));
    if (shader.push_constants.empty()) {
        if (!push.empty()) return std::unexpected(invalid("shader has no push constants"));
    } else {
        const auto range = shader.push_constants.front();
        if (range.offset || !range.size || range.size % 4 || range.size > limits.maxPushConstantsSize)
            return std::unexpected(unsupported("push constant layout exceeds baseline or device limits"));
        if (push.size() != range.size) return std::unexpected(invalid("push constant byte size does not match reflection"));
    }
    return {};
}
} // namespace dk::graphics::detail

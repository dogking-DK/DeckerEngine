#include <dk/graphics/ShaderArtifact.hpp>
#include <cstring>

namespace dk::graphics {
std::string_view shader_stage_name(ShaderStage stage) noexcept
{
    switch (stage) {
    case ShaderStage::vertex: return "vertex";
    case ShaderStage::fragment: return "fragment";
    case ShaderStage::compute: return "compute";
    }
    return "unknown";
}
std::string_view shader_descriptor_name(ShaderDescriptorType type) noexcept
{
    switch (type) {
    case ShaderDescriptorType::uniform_buffer: return "uniform_buffer";
    case ShaderDescriptorType::storage_buffer: return "storage_buffer";
    case ShaderDescriptorType::sampled_image: return "sampled_image";
    case ShaderDescriptorType::storage_image: return "storage_image";
    case ShaderDescriptorType::sampler: return "sampler";
    }
    return "unknown";
}
CompiledShader::CompiledShader(memory::ResourceHandle resource)
    : spirv(memory::Allocator<std::uint32_t>{resource}), entry(memory::Allocator<char>{resource}),
      compiler(memory::Allocator<char>{resource}), diagnostics(memory::Allocator<char>{resource}),
      bindings(memory::Allocator<ShaderBinding>{resource}), push_constants(memory::Allocator<ShaderPushConstant>{resource}),
      dependencies(memory::Allocator<String>{resource}) {}

namespace { Error invalid(std::string message) { return {ErrorCode::invalid_argument, std::move(message)}; } Error unsupported(std::string message) { return {ErrorCode::not_supported, std::move(message)}; } }
Result<void> validate_shader_artifact(const CompiledShader& shader)
{
    const auto stage = shader.stage;
    if ((stage != ShaderStage::vertex && stage != ShaderStage::fragment && stage != ShaderStage::compute) || shader.entry.empty() || shader.entry.find('\0') != String::npos)
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
} // namespace dk::graphics

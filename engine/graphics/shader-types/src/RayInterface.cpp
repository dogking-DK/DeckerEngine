#include <dk/graphics/ShaderArtifact.hpp>
#include <map>
#include <tuple>
#include <algorithm>
#include <limits>
#include <vector>
#include <span>

namespace dk::graphics {
namespace {
using Instruction = std::span<const std::uint32_t>;
Error unsupported() { return {ErrorCode::not_supported, "Unsupported RT interface: require 32-bit scalar/vector/struct/fixed-array payload and static SBT selectors"}; }
struct Type { std::string signature; std::uint32_t bytes; };
struct Parser {
    std::map<std::uint32_t, Instruction> types, constants, variables;
    std::map<std::uint32_t, std::uint32_t> locations;
    Result<std::uint32_t> constant(std::uint32_t id) const {
        const auto value = constants.find(id);
        if (value == constants.end() || value->second.size() != 4) return std::unexpected(unsupported());
        const auto type = types.find(value->second[1]);
        if (type == types.end() || (type->second[0]&0xffff) != 21 || type->second.size() != 4 || type->second[2] != 32)
            return std::unexpected(unsupported());
        return value->second[3];
    }
    Result<Type> type(std::uint32_t id, unsigned depth = 0) const {
        const auto found = types.find(id);
        if (found == types.end() || depth > 32) return std::unexpected(unsupported());
        const auto w = found->second; const auto op = w[0]&0xffff;
        if (op == 21 && w.size() == 4 && w[2] == 32 && w[3] <= 1) return Type{w[3] ? "i32" : "u32", 4};
        if (op == 22 && w.size() == 3 && w[2] == 32) return Type{"f32", 4};
        if ((op == 23 || op == 28) && w.size() == 4) {
            auto element = type(w[2], depth+1); if (!element) return element;
            auto count = op == 23 ? Result<std::uint32_t>{w[3]} : constant(w[3]);
            if (!count || !*count || *count > 4096 || element->bytes > UINT32_MAX / *count) return std::unexpected(unsupported());
            return Type{std::string{op == 23 ? "v" : "a"}+std::to_string(*count)+"("+element->signature+")", element->bytes * *count};
        }
        if (op == 30 && w.size() > 2 && w.size() <= 258) {
            Type result{"s(",0};
            for (std::size_t i = 2; i < w.size(); ++i) {
                auto field = type(w[i], depth+1);
                if (!field || field->bytes > UINT32_MAX-result.bytes || result.signature.size()+field->signature.size() > 65536)
                    return std::unexpected(unsupported());
                result.signature += field->signature+";"; result.bytes += field->bytes;
            }
            result.signature += ")"; return result;
        }
        return std::unexpected(unsupported());
    }
};
}
Result<ShaderRayInterface> reflect_ray_interface(const CompiledShader& shader)
{
    const auto memory = shader.spirv.get_allocator().resource();
    ShaderRayInterface result{memory};
    const auto& words = shader.spirv;
    if (words.size() < 5) return std::unexpected(unsupported());
    Parser parser;
    std::vector<Instruction> traces;
    for (std::size_t i = 5; i < words.size();) {
        const auto count = words[i]>>16, op = words[i]&0xffff;
        if (!count || count > words.size()-i) return std::unexpected(unsupported());
        const Instruction w{words.data()+i, count};
        if (op >= 19 && op <= 32 && count >= 2) parser.types.emplace(w[1], w);
        if (op == 43 && count >= 3) parser.constants.emplace(w[2], w);
        if (op == 59 && count >= 4) parser.variables.emplace(w[2], w);
        if (op == 71 && count == 4 && w[2] == 30) parser.locations.emplace(w[1], w[3]);
        if (op == 4445) traces.push_back(w);
        if (op == 4446) return std::unexpected(unsupported()); // ExecuteCallableKHR.
        i += count;
    }
    for (const auto& [id, w] : parser.variables) {
        const auto storage = w[3];
        if (storage != 5338 && storage != 5342 && storage != 5339) continue;
        if (!is_ray_tracing_stage(shader.stage)) return std::unexpected(unsupported());
        const auto pointer = parser.types.find(w[1]);
        if (pointer == parser.types.end() || pointer->second.size() != 4 || (pointer->second[0]&0xffff) != 32) return std::unexpected(unsupported());
        auto type = parser.type(pointer->second[3]); if (!type) return std::unexpected(type.error());
        const auto kind = storage == 5338 ? RayInterfaceKind::outgoing_payload : storage == 5342 ? RayInterfaceKind::incoming_payload : RayInterfaceKind::hit_attribute;
        const auto location = parser.locations.find(id);
        if (kind == RayInterfaceKind::outgoing_payload && location == parser.locations.end()) return std::unexpected(unsupported());
        if ((kind == RayInterfaceKind::outgoing_payload && shader.stage != ShaderStage::raygeneration) ||
            (kind == RayInterfaceKind::incoming_payload && shader.stage == ShaderStage::raygeneration) ||
            (kind == RayInterfaceKind::hit_attribute && (shader.stage != ShaderStage::closesthit && shader.stage != ShaderStage::anyhit)))
            return std::unexpected(unsupported());
        result.payloads.push_back({kind, location == parser.locations.end() ? 0 : location->second, type->bytes, String{type->signature, memory::Allocator<char>{memory}}});
    }
    std::ranges::sort(result.payloads, [](const auto& a, const auto& b) { return std::tie(a.kind,a.location)<std::tie(b.kind,b.location); });
    for (std::size_t i=1; i<result.payloads.size(); ++i)
        if (result.payloads[i-1].kind==result.payloads[i].kind && result.payloads[i-1].location==result.payloads[i].location) return std::unexpected(unsupported());
    for (const auto& w : traces) {
        if (shader.stage != ShaderStage::raygeneration || w.size() != 12) return std::unexpected(unsupported());
        auto offset=parser.constant(w[4]), stride=parser.constant(w[5]), miss=parser.constant(w[6]);
        const auto payload=parser.variables.find(w[11]);
        const auto location=parser.locations.find(w[11]);
        if (!offset || !stride || !miss || payload==parser.variables.end() || payload->second[3]!=5338 || location==parser.locations.end())
            return std::unexpected(unsupported());
        result.traces.push_back({*offset,*stride,*miss,location->second});
    }
    return result;
}
} // namespace dk::graphics

#include <dk/graphics/ShaderCompiler.hpp>
#include <dk/io/File.hpp>
#include <dk/io/Path.hpp>
#include <dk/profiling/Profiler.hpp>
#include <slang.h>
#include <slang-com-ptr.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cstring>
#include <limits>
#include <tuple>

namespace dk::graphics {
namespace {
using Slang::ComPtr;
using Category = slang::ParameterCategory;
using Kind = slang::TypeReflection::Kind;

bool identifier(std::string_view text)
{
    const auto alpha = [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; };
    return !text.empty() && alpha(text.front()) && std::ranges::all_of(text, [alpha](char c) {
        return alpha(c) || (c >= '0' && c <= '9');
    });
}
bool has_nul(std::string_view text) { return text.find('\0') != std::string_view::npos; }
SlangStage slang_stage(ShaderStage stage)
{
    switch (stage) {
    case ShaderStage::vertex: return SLANG_STAGE_VERTEX;
    case ShaderStage::fragment: return SLANG_STAGE_FRAGMENT;
    case ShaderStage::compute: return SLANG_STAGE_COMPUTE;
    }
    return SLANG_STAGE_NONE;
}
void append_diagnostics(String& text, const ComPtr<slang::IBlob>& blob)
{
    if (!blob || blob->getBufferSize() == 0) { return; }
    auto size = blob->getBufferSize();
    const auto* bytes = static_cast<const char*>(blob->getBufferPointer());
    if (bytes[size - 1] == '\0') { --size; }
    text.append(bytes, size);
}
bool bounded(std::size_t value) { return value > 0 && value <= std::numeric_limits<std::uint32_t>::max(); }

Result<void> reflect(slang::ProgramLayout* layout, CompiledShader& output)
{
    const auto fail = [](std::string detail) -> Result<void> {
        return std::unexpected(Error{ErrorCode::not_supported, "Unsupported shader layout: " + detail});
    };
    auto* entry = layout->getEntryPointByIndex(0);
    if (!entry || entry->getStage() != slang_stage(output.stage)) {
        return std::unexpected(Error{ErrorCode::invalid_argument, "Shader stage does not match the entry point attribute"});
    }
    // Entry parameters are stage IO only; resources/uniforms must be global.
    auto* entry_type = entry->getVarLayout()->getTypeLayout();
    for (unsigned i = 0; i < entry_type->getCategoryCount(); ++i) {
        const auto category = entry_type->getCategoryByIndex(i);
        if (category != Category::VaryingInput && category != Category::VaryingOutput) {
            return fail("entry-point uniform or resource parameters");
        }
    }
    if (output.stage == ShaderStage::compute) {
        SlangUInt threads[3]{};
        entry->getComputeThreadGroupSize(3, threads);
        for (std::size_t i = 0; i < 3; ++i) {
            if (!bounded(threads[i])) { return fail("unresolved compute thread group size"); }
            output.thread_group_size[i] = static_cast<std::uint32_t>(threads[i]);
        }
    }
    const auto resource = output.spirv.get_allocator().resource();
    for (unsigned i = 0; i < layout->getParameterCount(); ++i) {
        auto* parameter = layout->getParameterByIndex(i);
        auto* type = parameter->getTypeLayout();
        const std::string name = parameter->getName() ? parameter->getName() : "<unnamed>";
        if (type->getKind() == Kind::ConstantBuffer && type->getSize(Category::PushConstantBuffer) != 0) {
            auto* element = type->getElementTypeLayout();
            const auto size = element->getSize(Category::Uniform);
            if (!output.push_constants.empty() || !bounded(size) || size % 4 != 0
                || element->getBindingRangeCount() != 0) {
                return fail(name + ": only one plain, sized push constant block is supported");
            }
            output.push_constants.push_back({0, static_cast<std::uint32_t>(size)});
            continue;
        }
        auto* leaf = type;
        while (leaf->getKind() == Kind::Array) { leaf = leaf->getElementTypeLayout(); }
        if (leaf->getKind() != Kind::ConstantBuffer && leaf->getKind() != Kind::Resource
            && leaf->getKind() != Kind::SamplerState && leaf->getKind() != Kind::ShaderStorageBuffer) {
            return fail(name + ": global parameters must be resource bindings or a push constant block");
        }
        if (type->getBindingRangeCount() != 1 || type->isBindingRangeSpecializable(0)
            || type->getSize(Category::DescriptorTableSlot) == 0) {
            return fail(name + ": nested or specialized resource layout");
        }
        const auto count = type->getBindingRangeBindingCount(0);
        const auto binding = parameter->getOffset(Category::DescriptorTableSlot);
        const auto set = parameter->getBindingSpace(Category::DescriptorTableSlot);
        if (count <= 0 || !bounded(static_cast<std::size_t>(count))
            || binding >= std::numeric_limits<std::uint32_t>::max() || set >= std::numeric_limits<std::uint32_t>::max()) {
            return fail(name + ": unbounded or unresolved descriptor range");
        }
        ShaderBinding result{String{name, memory::Allocator<char>{resource}}, static_cast<std::uint32_t>(set),
            static_cast<std::uint32_t>(binding), ShaderDescriptorType::storage_buffer, static_cast<std::uint32_t>(count), 0};
        switch (type->getBindingRangeType(0)) {
        case slang::BindingType::ConstantBuffer: {
            const auto size = leaf->getElementTypeLayout()->getSize(Category::Uniform);
            if (!bounded(size) || leaf->getElementTypeLayout()->getBindingRangeCount() != 0) {
                return fail(name + ": constant buffer must contain only sized plain data");
            }
            result.type = ShaderDescriptorType::uniform_buffer;
            result.block_size = static_cast<std::uint32_t>(size);
            break;
        }
        case slang::BindingType::RawBuffer:
        case slang::BindingType::MutableRawBuffer: result.type = ShaderDescriptorType::storage_buffer; break;
        case slang::BindingType::Texture: result.type = ShaderDescriptorType::sampled_image; break;
        case slang::BindingType::MutableTexture: result.type = ShaderDescriptorType::storage_image; break;
        case slang::BindingType::Sampler: result.type = ShaderDescriptorType::sampler; break;
        default: return fail(name + ": unsupported descriptor type");
        }
        output.bindings.push_back(std::move(result));
    }
    std::ranges::sort(output.bindings, [](const ShaderBinding& a, const ShaderBinding& b) {
        return std::tie(a.set, a.binding) < std::tie(b.set, b.binding);
    });
    for (std::size_t i = 1; i < output.bindings.size(); ++i) {
        if (output.bindings[i - 1].set == output.bindings[i].set
            && output.bindings[i - 1].binding == output.bindings[i].binding) {
            return fail("duplicate descriptor set/binding");
        }
    }
    return {};
}
} // namespace

Result<CompiledShader> compile_shader(const ShaderCompileRequest& request, memory::ResourceHandle resource)
{
    DK_PROFILE_ZONE("Shader.compile");
    if (!resource || resource.state() != memory::ResourceState::open) {
        return std::unexpected(Error{ErrorCode::invalid_state, "Shader compilation requires an open Memory resource"});
    }
    if (request.source.empty() || !identifier(request.entry) || slang_stage(request.stage) == SLANG_STAGE_NONE) {
        return std::unexpected(Error{ErrorCode::invalid_argument, "Shader source, identifier entry and supported stage are required"});
    }
    std::error_code ec;
    const auto absolute = std::filesystem::absolute(request.source, ec);
    if (ec) { return std::unexpected(Error{ErrorCode::io_error, "Cannot resolve shader source: " + ec.message()}); }
    const auto path = path_to_utf8(absolute);
    if (!path) { return std::unexpected(path.error()); }
    if (has_nul(*path)) { return std::unexpected(Error{ErrorCode::invalid_argument, "NUL in shader path"}); }
    auto bytes = read_file_bytes(absolute);
    if (!bytes) { return std::unexpected(bytes.error()); }
    std::string source(bytes->size(), '\0');
    if (!bytes->empty()) { std::memcpy(source.data(), bytes->data(), bytes->size()); }
    if (has_nul(source)) { return std::unexpected(Error{ErrorCode::invalid_argument, "NUL in shader source"}); }
    CompiledShader output{resource};
    output.entry.assign(request.entry);
    output.stage = request.stage;
    const auto fail = [&](std::string operation, ErrorCode code = ErrorCode::invalid_argument) -> Result<CompiledShader> {
        if (!output.diagnostics.empty()) { operation += "\n" + std::string{output.diagnostics}; }
        return std::unexpected(Error{code, std::move(operation), {*path, "entry=" + std::string{request.entry},
            "stage=" + std::string{shader_stage_name(request.stage)}}});
    };
    std::vector<std::string> search_storage;
    for (const auto& include : request.include_paths) {
        auto text = path_to_utf8(include);
        if (!text) { return std::unexpected(text.error()); }
        if (text->empty() || has_nul(*text)) { return fail("Invalid include path"); }
        search_storage.push_back(std::move(*text));
    }
    std::vector<const char*> search_paths;
    for (const auto& text : search_storage) { search_paths.push_back(text.c_str()); }
    std::vector<std::pair<std::string, std::string>> define_storage;
    for (const auto& define : request.defines) {
        if (!identifier(define.name) || has_nul(define.value)) { return fail("Invalid preprocessor definition"); }
        for (const auto& previous : define_storage) {
            if (previous.first == define.name) { return fail("Duplicate preprocessor definition: " + previous.first); }
        }
        define_storage.emplace_back(define.name, define.value);
    }
    std::vector<slang::PreprocessorMacroDesc> defines;
    for (const auto& [name, value] : define_storage) { defines.push_back({name.c_str(), value.c_str()}); }
    ComPtr<slang::IGlobalSession> global;
    if (SLANG_FAILED(slang::createGlobalSession(global.writeRef()))) { return fail("Cannot create Slang global session", ErrorCode::internal_error); }
    output.compiler = global->getBuildTagString();
    // Slang may still return code after a missing optimizer diagnostic. Require the
    // selected backend up front so a broken deployment cannot silently change policy.
    if (SLANG_FAILED(global->checkPassThroughSupport(SLANG_PASS_THROUGH_SPIRV_OPT))) {
        return fail("Slang spirv-opt backend unavailable; deploy the matching slang-glslang runtime library", ErrorCode::not_supported);
    }
    slang::TargetDesc target{};
    target.format = SLANG_SPIRV;
    target.profile = global->findProfile("spirv_1_5");
    slang::CompilerOptionEntry options[4]{};
    options[0].name = slang::CompilerOptionName::EmitSpirvDirectly;
    options[0].value.intValue0 = 1;
    options[1].name = slang::CompilerOptionName::VulkanUseEntryPointName;
    options[1].value.intValue0 = 1;
    options[2].name = slang::CompilerOptionName::Optimization;
    options[2].value.intValue0 = SLANG_OPTIMIZATION_LEVEL_DEFAULT;
    options[3].name = slang::CompilerOptionName::DebugInformation;
    options[3].value.intValue0 = SLANG_DEBUG_INFO_LEVEL_NONE;
    slang::SessionDesc session_desc{};
    session_desc.targets = &target;
    session_desc.targetCount = 1;
    session_desc.defaultMatrixLayoutMode = SLANG_MATRIX_LAYOUT_ROW_MAJOR;
    session_desc.searchPaths = search_paths.data();
    session_desc.searchPathCount = static_cast<SlangInt>(search_paths.size());
    session_desc.preprocessorMacros = defines.data();
    session_desc.preprocessorMacroCount = static_cast<SlangInt>(defines.size());
    session_desc.compilerOptionEntries = options;
    session_desc.compilerOptionEntryCount = 4;
    ComPtr<slang::ISession> session;
    if (SLANG_FAILED(global->createSession(session_desc, session.writeRef()))) { return fail("Cannot create Slang session", ErrorCode::internal_error); }
    ComPtr<slang::IBlob> diagnostics;
    auto* module = session->loadModuleFromSourceString("dk_shader", path->c_str(), source.c_str(), diagnostics.writeRef());
    append_diagnostics(output.diagnostics, diagnostics);
    if (!module) { return fail("Slang source compilation failed"); }
    ComPtr<slang::IEntryPoint> entry;
    if (SLANG_FAILED(module->findEntryPointByName(output.entry.c_str(), entry.writeRef()))) {
        return fail("Entry point not found; declare a stage using [shader(...)] or a Slang-recognized stage attribute", ErrorCode::not_found);
    }
    slang::IComponentType* parts[] = {module, entry.get()};
    ComPtr<slang::IComponentType> program;
    auto status = session->createCompositeComponentType(parts, 2, program.writeRef(), diagnostics.writeRef());
    append_diagnostics(output.diagnostics, diagnostics);
    if (SLANG_FAILED(status)) { return fail("Slang composition failed"); }
    ComPtr<slang::IComponentType> linked;
    status = program->link(linked.writeRef(), diagnostics.writeRef());
    append_diagnostics(output.diagnostics, diagnostics);
    if (SLANG_FAILED(status)) { return fail("Slang linking failed"); }
    auto* layout = linked->getLayout(0, diagnostics.writeRef());
    append_diagnostics(output.diagnostics, diagnostics);
    if (!layout) { return fail("Slang layout failed"); }
    if (auto result = reflect(layout, output); !result) { return fail(result.error().message, result.error().code); }
    ComPtr<slang::IBlob> code;
    status = linked->getEntryPointCode(0, 0, code.writeRef(), diagnostics.writeRef());
    append_diagnostics(output.diagnostics, diagnostics);
    if (SLANG_FAILED(status)) { return fail("Slang SPIR-V generation failed"); }
    if (!code || code->getBufferSize() < 20 || code->getBufferSize() % sizeof(std::uint32_t) != 0) {
        return fail("Invalid SPIR-V artifact from Slang", ErrorCode::internal_error);
    }
    output.spirv.resize(code->getBufferSize() / sizeof(std::uint32_t));
    std::memcpy(output.spirv.data(), code->getBufferPointer(), code->getBufferSize());
    if (output.spirv[0] != 0x07230203U || output.spirv[1] != 0x00010500U) {
        return fail("Unexpected SPIR-V magic/version", ErrorCode::internal_error);
    }
    output.dependencies.emplace_back(*path, memory::Allocator<char>{resource});
    for (SlangInt32 i = 0; i < session->getLoadedModuleCount(); ++i) {
        auto* loaded = session->getLoadedModule(i);
        for (SlangInt32 j = 0; j < loaded->getDependencyFileCount(); ++j) {
            if (const auto* dependency = loaded->getDependencyFilePath(j)) {
                output.dependencies.emplace_back(dependency, memory::Allocator<char>{resource});
            }
        }
    }
    std::ranges::sort(output.dependencies);
    output.dependencies.erase(std::unique(output.dependencies.begin(), output.dependencies.end()), output.dependencies.end());
    return output;
}

String shader_reflection_json(const CompiledShader& shader)
{
    nlohmann::ordered_json json{{"schema_version", 1}, {"compiler", std::string_view{shader.compiler}},
        {"target", "spirv_1_5"}, {"matrix_layout", "row_major"}, {"entry", std::string_view{shader.entry}},
        {"stage", shader_stage_name(shader.stage)}, {"spirv_bytes", shader.spirv.size() * sizeof(std::uint32_t)},
        {"thread_group_size", shader.thread_group_size}, {"bindings", nlohmann::ordered_json::array()},
        {"push_constants", nlohmann::ordered_json::array()}};
    for (const auto& binding : shader.bindings) {
        json["bindings"].push_back({{"name", std::string_view{binding.name}}, {"set", binding.set},
            {"binding", binding.binding}, {"type", shader_descriptor_name(binding.type)}, {"count", binding.count},
            {"block_size", binding.block_size}});
    }
    for (const auto& range : shader.push_constants) {
        json["push_constants"].push_back({{"offset", range.offset}, {"size", range.size}});
    }
    return String{json.dump(2) + '\n', memory::Allocator<char>{shader.spirv.get_allocator().resource()}};
}
} // namespace dk::graphics

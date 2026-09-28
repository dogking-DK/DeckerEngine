#include <dk/graphics/ShaderCompiler.hpp>
#include <dk/io/File.hpp>
#include <dk/io/Path.hpp>
#include <dk/memory/MemorySystem.hpp>
#include <iostream>
#include <map>
#include <vector>

namespace {
void diagnostic(const dk::Error& error)
{
    std::cerr << dk::error_code_name(error.code) << ": " << error.message;
    for (const auto& context : error.context) { std::cerr << " [" << context << ']'; }
    std::cerr << '\n';
}
int run(const std::vector<std::string>& args)
{
    constexpr auto usage = "Usage: dk-shaderc compile --source FILE --entry NAME --stage vertex|fragment|compute --output FILE.spv\n"
        "                         [--include DIR ...] [--define NAME=VALUE ...]\n"
        "Writes SPIR-V atomically; reflection JSON goes to stdout, diagnostics to stderr.\n";
    if (args.size() == 1 && args[0] == "--help") { std::cout << usage; return 0; }
    if (args.empty() || args[0] != "compile" || args.size() % 2 != 1) { std::cerr << usage; return 2; }
    std::map<std::string, std::string> options;
    std::vector<std::filesystem::path> includes;
    std::vector<dk::graphics::ShaderDefine> defines;
    for (std::size_t i = 1; i < args.size(); i += 2) {
        const auto& key = args[i];
        const auto& value = args[i + 1];
        if (value.empty()) { std::cerr << usage; return 2; }
        if (key == "--include") {
            auto path = dk::path_from_utf8(value);
            if (!path) { diagnostic(path.error()); return 2; }
            includes.push_back(std::move(*path));
        } else if (key == "--define") {
            const auto separator = value.find('=');
            if (separator == std::string::npos || separator == 0) { std::cerr << usage; return 2; }
            defines.push_back({std::string_view{value}.substr(0, separator), std::string_view{value}.substr(separator + 1)});
        } else if ((key != "--source" && key != "--entry" && key != "--stage" && key != "--output")
                   || !options.emplace(key, value).second) { std::cerr << usage; return 2; }
    }
    if (options.size() != 4) { std::cerr << usage; return 2; }
    dk::graphics::ShaderStage stage{};
    if (options.at("--stage") == "vertex") { stage = dk::graphics::ShaderStage::vertex; }
    else if (options.at("--stage") == "fragment") { stage = dk::graphics::ShaderStage::fragment; }
    else if (options.at("--stage") == "compute") { stage = dk::graphics::ShaderStage::compute; }
    else { std::cerr << usage; return 2; }
    const auto source = dk::path_from_utf8(options.at("--source"));
    const auto output = dk::path_from_utf8(options.at("--output"));
    if (!source || !output) { diagnostic(!source ? source.error() : output.error()); return 2; }
    auto system_result = dk::memory::MemorySystem::create();
    if (!system_result) { std::cerr << "Cannot create Memory system\n"; return 3; }
    auto system = std::move(*system_result);
    auto heap = system.create_heap({"shaderc", dk::memory::DomainCategory::render});
    if (!heap) { std::cerr << "Cannot create shader Memory domain\n"; return 3; }
    auto compiled = dk::graphics::compile_shader({*source, options.at("--entry"), stage, includes, defines}, *heap);
    if (!compiled) { diagnostic(compiled.error()); return 1; }
    // Protect source, includes and imported modules, including hard-link aliases.
    std::error_code ec;
    const bool output_exists = std::filesystem::exists(*output, ec);
    if (ec) { std::cerr << "Cannot inspect output: " << ec.message() << '\n'; return 1; }
    if (output_exists) {
        for (const auto& dependency : compiled->dependencies) {
            auto path = dk::path_from_utf8(dependency);
            if (!path) { diagnostic(path.error()); return 1; }
            const bool same = std::filesystem::equivalent(*path, *output, ec);
            if (ec) { std::cerr << "Cannot inspect shader dependency: " << ec.message() << '\n'; return 1; }
            if (same) { std::cerr << "Output cannot overwrite shader source or dependency\n"; return 1; }
        }
    }
    const auto json = dk::graphics::shader_reflection_json(*compiled);
    if (!compiled->diagnostics.empty()) { std::cerr << compiled->diagnostics; }
    const auto result = dk::write_file_bytes_atomic(*output, std::as_bytes(std::span{compiled->spirv}));
    if (!result) { diagnostic(result.error()); return 1; }
    std::cout << json;
    std::cout.flush();
    if (!std::cout) { std::cerr << "SPIR-V committed but reflection stdout write failed\n"; return 1; }
    return 0;
}
} // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t** argv)
#else
int main(int argc, char** argv)
#endif
{
    try {
        std::vector<std::string> args;
        for (int i = 1; i < argc; ++i) {
#ifdef _WIN32
            auto text = dk::path_to_utf8(std::filesystem::path{argv[i]});
            if (!text) { std::cerr << "Invalid Unicode argument\n"; return 2; }
            args.push_back(std::move(*text));
#else
            args.emplace_back(argv[i]);
#endif
        }
        return run(args);
    } catch (const std::exception& error) {
        std::cerr << "shaderc infrastructure failure: " << error.what() << '\n';
        return 3;
    }
}

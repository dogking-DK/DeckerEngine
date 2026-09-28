#include <dk/assets/AssetCompiler.hpp>
#include <dk/assets/AssetCache.hpp>
#include <dk/memory/MemorySystem.hpp>
#include <dk/memory/Context.hpp>
#include <charconv>
#include <cmath>
#include <iostream>
#include <map>
#include <vector>

namespace {
void diagnostic(const dk::Error& error) { std::cerr << dk::error_code_name(error.code) << ": " << error.message; for (const auto& context : error.context) { std::cerr << " [" << context << "]"; } std::cerr << '\n'; }
int run(const std::vector<std::string>& args)
{
    constexpr auto usage = "Usage: dk-assetc import --project-root ROOT --source REL --output REL [--unit-scale NUMBER]\n"
        "       dk-assetc cache --project-root ROOT --source REL [--unit-scale NUMBER]\n";
    if (args.size() == 1 && args[0] == "--help") { std::cerr << usage; return 0; }
    if (args.empty() || (args[0] != "import" && args[0] != "cache") || args.size() % 2 != 1) { std::cerr << usage; return 2; }
    std::map<std::string, std::string> options;
    for (std::size_t i = 1; i < args.size(); i += 2) {
        const auto& key = args[i];
        if ((key != "--project-root" && key != "--source" && key != "--output" && key != "--unit-scale")
            || args[i + 1].empty() || !options.emplace(key, args[i + 1]).second) { std::cerr << usage; return 2; }
    }
    if (!options.contains("--project-root") || !options.contains("--source")
        || (options.contains("--output") != (args[0] == "import"))) { std::cerr << usage; return 2; }
    std::optional<double> scale;
    if (options.contains("--unit-scale")) {
        const auto& text = options.at("--unit-scale"); double value = 0;
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
        if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || !std::isfinite(value) || value <= 0) { std::cerr << usage; return 2; }
        scale = value;
    }
    auto system_result = dk::memory::MemorySystem::create();
    if (!system_result) { std::cerr << "Cannot create memory system\n"; return 3; }
    auto system = std::move(*system_result);
    auto heap = system.create_heap({"assetc", dk::memory::DomainCategory::assets});
    if (!heap) { std::cerr << "Cannot create Assets memory domain\n"; return 3; }
    dk::memory::ThreadContext context{system, *heap};
    dk::memory::ExecutionScope scope{context, *heap};
    const auto root = dk::path_from_utf8(options.at("--project-root"));
    if (!root) { diagnostic(root.error()); return 1; }
    const auto paths = dk::ProjectPaths::create(*root);
    if (!paths) { diagnostic(paths.error()); return 1; }
    if (args[0] == "cache") {
        const auto result = dk::compile_cached_asset(*paths, {options.at("--source"), scale});
        if (!result) { diagnostic(result.error()); return 1; }
        std::cout << result->summary_json; return 0;
    }
    const auto result = dk::compile_asset(*paths, {options.at("--source"), options.at("--output"), scale, {}});
    if (!result) { diagnostic(result.error()); return 1; }
    std::cout << result->summary_json;
    return 0;
}
}
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
    } catch (const std::exception& error) { std::cerr << "assetc infrastructure failure: " << error.what() << '\n'; return 3; }
}

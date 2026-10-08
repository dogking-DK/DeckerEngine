#include <dk/core/Version.hpp>
#include <dk/profiling/Profiler.hpp>
#ifdef DK_RUN_WITH_LUAU
#include <dk/scripting/Luau.hpp>
#endif
#ifdef DK_RUN_WITH_RUNTIME
#include <dk/automation/JsonLines.hpp>
#include <fstream>
#ifdef _WIN32
#include <dk/automation/IpcServer.hpp>
#include <fcntl.h>
#include <io.h>
#endif
#endif
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace
{
int run(const std::vector<std::filesystem::path> &args)
{
    if (args.empty() || (args.size() == 1 && args[0] == "--help"))
    {
        std::cout << "DeckerEngine runner\nUsage: dk-run [--help | --version]\n";
#ifdef DK_RUN_WITH_RUNTIME
        std::cout << "       dk-run --project-root ROOT --batch FILE [--auto-guard]\n";
        std::cout << "       dk-run --project-root ROOT --stdio\n";
#ifdef DK_RUN_WITH_LUAU
        std::cout << "       dk-run --project-root ROOT --script FILE.luau\n";
#endif
#ifdef _WIN32
        std::cout << "       dk-run --project-root ROOT --pipe NAME\n";
#endif
#endif
        return 0;
    }
    if (args.size() == 1 && args[0] == "--version")
    {
        std::cout << "DeckerEngine " << dk::version() << '\n';
        return 0;
    }
#ifdef DK_RUN_WITH_RUNTIME
    std::filesystem::path root, batch, script;
    std::string pipe_name;
    bool auto_guard = false, stdio = false;
    for (std::size_t i = 0; i < args.size(); ++i)
    {
        if (args[i] == "--project-root" && root.empty() && i + 1 < args.size())
            root = args[++i];
        else if (args[i] == "--batch" && batch.empty() && i + 1 < args.size())
            batch = args[++i];
#ifdef DK_RUN_WITH_LUAU
        else if (args[i] == "--script" && script.empty() && i + 1 < args.size())
            script = args[++i];
#endif
        else if (args[i] == "--auto-guard" && !auto_guard)
            auto_guard = true;
        else if (args[i] == "--stdio" && !stdio)
            stdio = true;
#ifdef _WIN32
        else if (args[i] == "--pipe" && pipe_name.empty() && i + 1 < args.size())
        {
            const auto name = args[++i].u8string();
            pipe_name.assign(reinterpret_cast<const char*>(name.data()), name.size());
            if (!dk::ipc::valid_endpoint(pipe_name)) { std::cerr << "Invalid pipe name.\n"; return 2; }
        }
#endif
        else
        {
            std::cerr << "Invalid arguments. Use dk-run --help.\n";
            return 2;
        }
    }
    if (root.empty() || (static_cast<int>(stdio) + static_cast<int>(!batch.empty()) + static_cast<int>(!pipe_name.empty()) + static_cast<int>(!script.empty()) != 1)
        || (auto_guard && batch.empty()))
    {
        std::cerr << "Use --project-root with exactly one input mode; --auto-guard is batch-only.\n";
        return 2;
    }
    auto runtime = dk::Runtime::create(root);
    if (!runtime)
    {
        std::cerr << runtime.error().message << '\n';
        return 2;
    }
#ifdef DK_RUN_WITH_LUAU
    if (!script.empty())
    {
        if (script.extension() != ".luau")
        {
            std::cerr << "Script input must be a .luau source file.\n";
            return 2;
        }
        std::ifstream input(script, std::ios::binary);
        if (!input) { std::cerr << "Cannot open script file.\n"; return 2; }
        const std::string source{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
        if (input.bad()) { std::cerr << "Cannot read script file.\n"; return 2; }
        const auto path = script.u8string();
        auto result = dk::run_luau(**runtime, source,
            std::string_view(reinterpret_cast<const char*>(path.data()), path.size()));
        if (!result) { std::cerr << result.error().message << '\n'; return 2; }
        return 0;
    }
#endif
#ifdef _WIN32
    if (_setmode(_fileno(stdout), _O_BINARY) == -1 || (stdio && _setmode(_fileno(stdin), _O_BINARY) == -1))
    {
        std::cerr << "Cannot configure binary protocol streams.\n";
        return 3;
    }
#endif
#ifdef _WIN32
    if (!pipe_name.empty())
    {
        auto server = dk::IpcServer::listen(**runtime, pipe_name);
        if (!server) { std::cerr << server.error().message << '\n'; return 3; }
        while (!(*runtime)->stopping())
        {
            const auto sequence = (*runtime)->events()->sequence();
            (*runtime)->pump();
            (void)(*server)->pump();
            if (!(*runtime)->stopping()) (*runtime)->events()->wait(sequence, std::chrono::steady_clock::now() + std::chrono::seconds{1});
        }
        (*server)->close(std::chrono::milliseconds{1500});
        return 0;
    }
#endif
    if (stdio)
        return dk::run_stdio(**runtime, std::cout, std::cerr);
    std::ifstream input(batch, std::ios::binary);
    if (!input)
    {
        std::cerr << "Cannot open batch file.\n";
        return 2;
    }
    return dk::run_json_lines(**runtime, input, std::cout, std::cerr, auto_guard);
#else
    std::cerr << "Unsupported arguments. Use dk-run --help.\n";
    return 2;
#endif
}
template <typename Char> int entry(int argc, Char **argv)
{
    try
    {
        DK_PROFILE_THREAD_NAME("dk-run main");
        DK_PROFILE_ZONE("Runner.Entry");
        std::vector<std::filesystem::path> args;
        for (int i = 1; i < argc; ++i)
            args.emplace_back(argv[i]);
        return run(args);
    }
    catch (const std::exception &e)
    {
        std::cerr << "Fatal runner error: " << e.what() << '\n';
        return 3;
    }
    catch (...)
    {
        std::cerr << "Fatal runner error\n";
        return 3;
    }
}
} // namespace
#ifdef _WIN32
int wmain(int argc, wchar_t **argv)
{
    return entry(argc, argv);
}
#else
int main(int argc, char **argv)
{
    return entry(argc, argv);
}
#endif

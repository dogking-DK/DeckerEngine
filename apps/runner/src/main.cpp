#include <dk/core/Version.hpp>
#ifdef DK_RUN_WITH_RUNTIME
#include <dk/automation/JsonLines.hpp>
#include <fstream>
#ifdef _WIN32
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
        std::cout << "DeckerEngine CPU runner\nUsage: dk-run [--help | --version]\n";
#ifdef DK_RUN_WITH_RUNTIME
        std::cout << "       dk-run --project-root ROOT --batch FILE [--auto-guard]\n";
        std::cout << "       dk-run --project-root ROOT --stdio\n";
#endif
        return 0;
    }
    if (args.size() == 1 && args[0] == "--version")
    {
        std::cout << "DeckerEngine " << dk::version() << '\n';
        return 0;
    }
#ifdef DK_RUN_WITH_RUNTIME
    std::filesystem::path root, batch;
    bool auto_guard = false, stdio = false;
    for (std::size_t i = 0; i < args.size(); ++i)
    {
        if (args[i] == "--project-root" && root.empty() && i + 1 < args.size())
            root = args[++i];
        else if (args[i] == "--batch" && batch.empty() && i + 1 < args.size())
            batch = args[++i];
        else if (args[i] == "--auto-guard" && !auto_guard)
            auto_guard = true;
        else if (args[i] == "--stdio" && !stdio)
            stdio = true;
        else
        {
            std::cerr << "Invalid arguments. Use dk-run --help.\n";
            return 2;
        }
    }
    if (root.empty() || (stdio == !batch.empty()) || (stdio && auto_guard))
    {
        std::cerr << "Use --project-root with exactly one of --batch/--stdio; --auto-guard is batch-only.\n";
        return 2;
    }
    auto runtime = dk::Runtime::create(root);
    if (!runtime)
    {
        std::cerr << runtime.error().message << '\n';
        return 2;
    }
#ifdef _WIN32
    if (_setmode(_fileno(stdout), _O_BINARY) == -1 || (stdio && _setmode(_fileno(stdin), _O_BINARY) == -1))
    {
        std::cerr << "Cannot configure binary protocol streams.\n";
        return 3;
    }
#endif
    if (stdio)
        return dk::run_json_lines(**runtime, std::cin, std::cout, std::cerr, false, true);
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

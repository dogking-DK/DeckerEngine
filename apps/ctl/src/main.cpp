#include <dk/automation/Client.hpp>
#include <dk/core/StableId.hpp>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <vector>
#include <fcntl.h>
#include <io.h>

namespace
{
std::string utf8(const std::filesystem::path& value)
{
    const auto text = value.u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}
std::optional<std::int64_t> number(std::string_view value)
{
    std::int64_t n{};
    const auto result = std::from_chars(value.data(), value.data() + value.size(), n);
    if (result.ec != std::errc{} || result.ptr != value.data() + value.size() || n <= 0) return {};
    return n;
}
int usage(std::string message)
{
    std::cout << dk::Json{{"status", "usage_error"}, {"execution", "not_sent"}, {"message", std::move(message)}}.dump() << '\n';
    return 2;
}
int run(const std::vector<std::filesystem::path>& args)
{
    if (args.size() == 1 && args.front() == "--help")
    {
        std::cout << "Usage: dk-ctl --pipe NAME --method METHOD [--params JSON | --params-file FILE]\n"
            "              [--timeout-ms 1..60000] [--session UUID --request-id N]\n"
            "       dk-ctl --pipe NAME --hello [--timeout-ms 1..60000]\n"
            "Retries require the original session, request id, method and parameters. No automatic retry.\n";
        return 0;
    }
    std::map<std::string, std::filesystem::path> values;
    bool hello = false;
    for (std::size_t i = 0; i < args.size(); ++i)
    {
        const auto key = utf8(args[i]);
        if (key == "--hello" && !hello) { hello = true; continue; }
        if (key != "--pipe" && key != "--method" && key != "--params" && key != "--params-file" &&
            key != "--timeout-ms" && key != "--session" && key != "--request-id") return usage("Unknown or duplicate option");
        if (values.contains(key) || i + 1 == args.size()) return usage("Duplicate option or missing value");
        values.emplace(key, args[++i]);
    }
    if (!values.contains("--pipe") || hello == values.contains("--method") ||
        (values.contains("--params") && values.contains("--params-file")) ||
        values.contains("--session") != values.contains("--request-id") ||
        (hello && (values.contains("--params") || values.contains("--params-file") || values.contains("--session"))))
        return usage("Use --pipe and either --hello or --method; retry needs both --session and --request-id");
    dk::ipc::ClientOptions options;
    options.endpoint = utf8(values.at("--pipe"));
    if (!dk::ipc::valid_endpoint(options.endpoint)) return usage("Invalid endpoint name");
    if (!hello && (utf8(values.at("--method")).empty() || utf8(values.at("--method")).size() > 256))
        return usage("Method must contain 1..256 UTF-8 bytes");
    if (values.contains("--timeout-ms"))
    {
        const auto timeout = number(utf8(values.at("--timeout-ms")));
        if (!timeout || *timeout > 60000) return usage("Timeout must be 1..60000 ms");
        options.timeout = std::chrono::milliseconds{*timeout};
    }
    if (values.contains("--session"))
    {
        const auto id = number(utf8(values.at("--request-id")));
        if (!id) return usage("Request id must be a positive int64");
        options.retry = dk::ipc::RetryTicket{utf8(values.at("--session")), *id};
        struct SessionTag;
        const auto session = dk::StableId<SessionTag>::parse(options.retry->session);
        if (!session || session->is_nil()) return usage("Session must be a non-nil UUID in standard hyphenated format");
    }
    std::string params = "{}";
    if (values.contains("--params")) params = utf8(values.at("--params"));
    if (values.contains("--params-file"))
    {
        std::ifstream input(values.at("--params-file"), std::ios::binary);
        if (!input) return usage("Cannot open parameters file");
        params.resize(dk::ipc::max_request_bytes + 1);
        input.read(params.data(), static_cast<std::streamsize>(params.size()));
        params.resize(static_cast<std::size_t>(input.gcount()));
        if (input.bad() || params.size() > dk::ipc::max_request_bytes) return usage("Cannot read parameters or file exceeds 1 MiB");
    }
    if (params.starts_with("\xEF\xBB\xBF")) return usage("Parameters must be UTF-8 JSON without BOM");
    const auto parsed = dk::parse_command_json(params);
    if (!parsed || !parsed->is_object()) return usage("Parameters must be a UTF-8 JSON object without BOM");
    auto reply = hello ? dk::ipc::discover(options) : dk::ipc::call(options, utf8(values.at("--method")), *parsed);
    std::cout << reply.json().dump() << '\n';
    return reply.exit_code();
}
} // namespace
int wmain(int argc, wchar_t** argv)
{
    try
    {
        if (_setmode(_fileno(stdout), _O_BINARY) == -1) return 3;
        std::vector<std::filesystem::path> args;
        for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);
        return run(args);
    }
    catch (const std::exception& error)
    {
        // Exceptional failures after dispatch cannot establish whether the command ran.
        std::cout << dk::Json{{"status", "system_error"}, {"execution", "unknown"}, {"message", error.what()}}.dump() << '\n';
        return 3;
    }
}

#include <dk/automation/JsonLines.hpp>
#include <istream>
#include <ostream>

namespace dk
{
int run_json_lines(Runtime &runtime, std::istream &input, std::ostream &output, std::ostream &diagnostics,
                   bool auto_guard)
{
    bool failed = false;
    for (;;)
    {
        std::string line;
        bool oversized = false, received = false;
        char ch = 0;
        while (input.get(ch))
        {
            received = true;
            if (ch == '\n')
                break;
            if (line.size() < 1024 * 1024)
                line.push_back(ch);
            else
                oversized = true;
        }
        if (input.bad() || (input.fail() && !input.eof()))
        {
            diagnostics << "Input stream failure\n";
            return 3;
        }
        if (!received && input.eof())
            break;
        if (!oversized && line.find_first_not_of(" \t\r") == std::string::npos)
            continue;
        auto result = oversized ? RpcOutcome{rpc_error(nullptr, -32700, "JSON line exceeds 1 MiB"), true}
                                : dispatch_json_line(runtime, line, auto_guard);
        failed = failed || result.failed;
        if (result.response)
        {
            output << result.response->dump() << '\n';
            output.flush();
            if (!output)
            {
                diagnostics << "Output stream failure\n";
                return 3;
            }
        }
    }
    return failed ? 1 : 0;
}
} // namespace dk

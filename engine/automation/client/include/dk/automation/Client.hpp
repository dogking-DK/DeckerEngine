#pragma once
#include <dk/automation/NamedPipe.hpp>
#include <dk/commands/Schema.hpp>
#include <cstdint>

namespace dk::ipc
{
struct RetryTicket { std::string session; std::int64_t request_id = 0; };
struct ClientOptions
{
    std::string endpoint;
    std::chrono::milliseconds timeout{5000};
    std::optional<RetryTicket> retry;
};
struct ClientReply
{
    std::string status = "ok";
    // not_sent: no command frame write attempted. unknown: it may have executed.
    std::string execution = "not_sent";
    std::optional<RetryTicket> ticket;
    std::optional<Json> response;
    std::optional<Json> hello;
    std::string message;
    [[nodiscard]] Json json() const;
    [[nodiscard]] int exit_code() const;
};
[[nodiscard]] ClientReply discover(const ClientOptions&);
[[nodiscard]] ClientReply call(const ClientOptions&, std::string_view method, const Json& params = Json::object());
} // namespace dk::ipc

#include <dk/profiling/Profiler.hpp>

#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>

namespace {
using namespace std::chrono_literals;

bool wait_for_connection(bool expected)
{
    const auto deadline = std::chrono::steady_clock::now() + 15s;
    while (dk::profiling::is_connected() != expected) {
        if (std::chrono::steady_clock::now() >= deadline) { return false; }
        std::this_thread::sleep_for(2ms);
    }
    return true;
}

std::uint64_t work()
{
    DK_PROFILE_ZONE("Probe.Worker");
    std::uint64_t sum = 0;
    for (std::uint64_t index = 0; index < 128; ++index) {
        DK_PROFILE_ZONE("Probe.Step");
        DK_PROFILE_ZONE_VALUE(index);
        sum += index;
        // Keep each verification zone above the timer's resolution. This is a
        // capture correctness probe, not a performance benchmark.
        std::this_thread::sleep_for(100us);
    }
    return sum;
}

void exception_scope()
{
    DK_PROFILE_ZONE("Probe.Exception");
    std::this_thread::sleep_for(100us);
    throw 7;
}

void text_inputs()
{
    {
        DK_PROFILE_ZONE("Probe.TemporaryText");
        DK_PROFILE_ZONE_TEXT(std::string(80, 't'));
        std::this_thread::sleep_for(100us);
    }
    {
        DK_PROFILE_ZONE("Probe.EmptyText");
        DK_PROFILE_ZONE_TEXT(std::string_view{});
        std::this_thread::sleep_for(100us);
    }
    {
        DK_PROFILE_ZONE("Probe.LongText");
        DK_PROFILE_ZONE_TEXT(std::string(65536, 'x'));
        std::this_thread::sleep_for(100us);
    }
}
} // namespace

int main(int argc, char** argv)
{
    const bool capture = argc == 2 && std::string_view{argv[1]} == "--capture";
    if (argc != 1 && !capture) { return 2; }
    DK_PROFILE_THREAD_NAME("dk-profile-main");
    if (capture && (!dk::profiling::enabled() || !wait_for_connection(true))) {
        std::cerr << "Profiling connection was not established within 15 seconds.\n";
        return 3;
    }
    std::uint64_t checksum = 0;
    {
        DK_PROFILE_ZONE("Probe.Capture");
        int text_evaluations = 0;
        DK_PROFILE_ZONE_TEXT((++text_evaluations, std::string_view{"controlled capture"}));
        if (text_evaluations != (dk::profiling::enabled() ? 1 : 0)) { return 4; }
        std::jthread worker{[&checksum] {
            DK_PROFILE_THREAD_NAME("dk-profile-worker");
            checksum = work();
        }};
        worker.join();
        text_inputs();
        try { exception_scope(); }
        catch (int) { /* Test zone closure during unwinding. */ }
    }
    DK_PROFILE_FRAME("Probe.Frame");
    // The capture tool stops after a bounded interval; normal execution continues.
    if (capture && !wait_for_connection(false)) {
        std::cerr << "Profiling client did not disconnect within 15 seconds.\n";
        return 5;
    }
    {
        DK_PROFILE_ZONE("Probe.AfterDisconnect");
        std::cout << "profiling=" << (dk::profiling::enabled() ? "on" : "off")
                  << ";checksum=" << checksum << '\n';
    }
    return checksum == 8128 ? 0 : 6;
}

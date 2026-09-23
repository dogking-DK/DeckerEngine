#include <dk/memory/Pool.hpp>
#include <dk/memory/MemorySystem.hpp>
#include <dk/profiling/Memory.hpp>
#include <array>
#include <barrier>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <thread>

namespace {
using namespace dk::memory;
using namespace std::chrono_literals;
bool wait_connection(bool expected)
{
    const auto deadline = std::chrono::steady_clock::now() + 15s;
    while (dk::profiling::is_connected() != expected) {
        if (std::chrono::steady_clock::now() >= deadline) { return false; }
        std::this_thread::sleep_for(2ms);
    }
    return true;
}
void require(bool value) { if (!value) { throw std::runtime_error("pool probe check failed"); } }
struct Measurement { PoolSnapshot local_live, local_idle, local_final, shared_live, shared_idle, shared_final; };
Measurement workload()
{
    DK_PROFILE_ZONE("Pool.Probe");
    auto owner = MemorySystem::create(); require(owner.has_value());
    auto heap = owner->create_heap({"pool probe", DomainCategory::jobs}); require(heap.has_value());
    Measurement measurement;
    {
        LocalPoolResource local{*heap, {16, 256}};
        auto created = SharedPoolResource::create(*heap, {16, 256}); require(created.has_value()); auto shared = *created;
        std::array<void*, 64> local_blocks{}, shared_blocks{};
        for (std::size_t i = 0; i < local_blocks.size(); ++i) {
            auto a = local.try_allocate(8, 8); auto b = shared.try_allocate(8, 8); require(a && b);
            local_blocks[i] = *a; shared_blocks[i] = *b;
        }
        require(local.try_sample().has_value()); measurement.local_live = local.snapshot();
        require(shared.try_sample().has_value());
        std::barrier sync{2};
        std::jthread worker{[&] {
            DK_PROFILE_THREAD_NAME("dk-pool-worker");
            DK_PROFILE_ZONE("Pool.Worker");
            for (auto* pointer : shared_blocks) { shared.deallocate(pointer, 8, 8); }
            for (auto& pointer : shared_blocks) { auto p = shared.try_allocate(16, 8); require(p.has_value()); pointer = *p; }
            require(shared.try_sample().has_value()); measurement.shared_live = shared.snapshot();
            sync.arrive_and_wait(); sync.arrive_and_wait();
            for (auto* pointer : shared_blocks) { shared.deallocate(pointer, 16, 8); }
        }};
        sync.arrive_and_wait(); sync.arrive_and_wait(); worker.join();
        for (auto* pointer : local_blocks) { local.deallocate(pointer, 8, 8); }
        require(local.try_sample().has_value()); measurement.local_idle = local.snapshot();
        require(shared.try_sample().has_value()); measurement.shared_idle = shared.snapshot();
        require(local.try_trim().has_value()); require(shared.try_trim().has_value());
        measurement.local_final = local.snapshot(); measurement.shared_final = shared.snapshot();
        require(measurement.local_live.logical_live_bytes == 512 && measurement.shared_live.logical_live_bytes == 1024);
        require(measurement.local_final.backing_bytes == 0 && measurement.shared_final.backing_bytes == 0);
    }
    require(owner->try_close().closed());
    require(measurement.local_final.backing_allocation_count + measurement.shared_final.backing_allocation_count < 192);
    return measurement;
}
}
int main(int argc, char** argv)
{
    const bool capture = argc == 2 && std::string_view{argv[1]} == "--capture";
    if (argc != 1 && !capture) { return 2; }
    DK_PROFILE_THREAD_NAME("dk-pool-main");
    try {
        if (capture && (!dk::profiling::enabled() || !wait_connection(true))) { return 3; }
        const auto m = workload();
        if (capture && !wait_connection(false)) { return 4; }
        // Standard pool chunk layouts vary by STL/configuration; report actual backing independently of Tracy.
        std::cout << "{\"status\":\"passed\",\"memory_enabled\":" << (dk::profiling::memory_enabled() ? "true" : "false")
            << ",\"requests\":192,\"backing_allocations\":" << m.local_final.backing_allocation_count + m.shared_final.backing_allocation_count
            << ",\"backing_bytes_total\":" << m.local_final.total_backing_bytes + m.shared_final.total_backing_bytes
            << ",\"local_backing_peak\":" << m.local_live.backing_bytes << ",\"local_idle_peak\":" << m.local_idle.idle_backing_bytes
            << ",\"shared_backing_peak\":" << m.shared_live.backing_bytes << ",\"shared_idle_peak\":" << m.shared_idle.idle_backing_bytes
            << "}\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}

#include <dk/memory/Context.hpp>
#include <dk/memory/MemorySystem.hpp>
#include <dk/profiling/Memory.hpp>
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
void require(bool value) { if (!value) { throw std::runtime_error("arena probe check failed"); } }
void allocate(ScratchArena& arena, std::size_t bytes)
{ require(arena.try_allocate(bytes, 8).has_value()); }
void workload()
{
    DK_PROFILE_ZONE("Arena.Probe");
    auto owner = MemorySystem::create(); require(owner.has_value());
    auto upstream = owner->create_heap({"probe scratch", DomainCategory::jobs}); require(upstream.has_value());
    {
        ThreadContext context{*owner, *upstream, {1024, 1024}}; ExecutionScope execution{context, *upstream};
        auto& arena = context.scratch();
        {
            ScratchScope outer;
            for (int i = 0; i < 100; ++i) { allocate(arena, 8); }
            {
                ScratchScope inner; allocate(arena, 2048);
                std::barrier sync{2};
                std::jthread worker{[&] {
                    DK_PROFILE_THREAD_NAME("dk-arena-worker");
                    ThreadContext local{*owner, *upstream, {128, 128}}; ExecutionScope entry{local, *upstream};
                    ScratchScope temporary; allocate(local.scratch(), 64);
                    local.scratch().sample();
                    sync.arrive_and_wait(); sync.arrive_and_wait();
                }};
                sync.arrive_and_wait(); arena.sample(); sync.arrive_and_wait(); worker.join();
                require(arena.snapshot().used_bytes == 2848);
            }
            require(arena.snapshot().used_bytes == 800);
        }
        require(arena.snapshot().retained_bytes == 1024);
        require(arena.try_reset().has_value());
    }
    require(upstream->snapshot().allocation_count == 3);
    require(owner->try_close().closed());
}
}
int main(int argc, char** argv)
{
    const bool capture = argc == 2 && std::string_view{argv[1]} == "--capture";
    if (argc != 1 && !capture) { return 2; }
    DK_PROFILE_THREAD_NAME("dk-arena-main");
    try {
        if (capture && (!dk::profiling::enabled() || !wait_connection(true))) { return 3; }
        workload();
        if (capture && !wait_connection(false)) { return 4; }
        std::cout << "arena=" << (dk::profiling::memory_enabled() ? "on" : "off") << ";chunks=3;requests=102\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}

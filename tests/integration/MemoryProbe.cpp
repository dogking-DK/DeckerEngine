#include <dk/memory/MemorySystem.hpp>
#include <dk/profiling/Memory.hpp>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <thread>

namespace {
using namespace std::chrono_literals;
using namespace dk::memory;
bool wait_connection(bool expected)
{
    const auto deadline = std::chrono::steady_clock::now() + 15s;
    while (dk::profiling::is_connected() != expected) {
        if (std::chrono::steady_clock::now() >= deadline) { return false; }
        std::this_thread::sleep_for(2ms);
    }
    return true;
}
MemorySystem system()
{
    auto value = MemorySystem::create();
    if (!value) { throw std::runtime_error("system creation failed"); }
    return std::move(*value);
}
ResourceHandle heap(MemorySystem& owner, HeapOptions options)
{
    auto value = owner.create_heap(options);
    if (!value) { throw std::runtime_error("heap creation failed"); }
    return *value;
}
void* allocate(const ResourceHandle& owner, std::size_t bytes)
{
    auto value = owner.try_allocate(bytes, 64);
    if (!value) { throw std::runtime_error("allocation failed"); }
    return *value;
}
void workload()
{
    DK_PROFILE_ZONE("Memory.Probe");
    auto primary = system();
    auto assets = heap(primary, {"main assets", DomainCategory::assets, 256});
    auto scene = heap(primary, {"scene", DomainCategory::scene, 64});
    void* remote = nullptr;
    std::jthread producer{[&] {
        DK_PROFILE_THREAD_NAME("dk-memory-worker");
        auto result = assets.try_allocate(128, 64);
        if (result) { remote = *result; }
    }};
    producer.join();
    if (!remote) { throw std::runtime_error("worker allocation failed"); }
    assets.deallocate(remote, 128, 64);
    auto denied = assets.try_allocate(512, 64);
    if (denied || denied.error() != AllocationError::limit_exceeded) { throw std::runtime_error("budget check failed"); }
    void* zero = allocate(assets, 0);
    std::jthread consumer{[&] { assets.deallocate(zero, 0, 64); }};
    consumer.join();
    for (int i = 0; i < 32; ++i) {
        void* pointer = allocate(assets, 64);
        assets.deallocate(pointer, 64, 64);
    }
    ResourceHandle delayed;
    void* retained = nullptr;
    {
        auto secondary = system();
        delayed = heap(secondary, {"second runtime assets", DomainCategory::assets, 64});
        retained = allocate(delayed, 64);
    }
    if (delayed.snapshot().state != ResourceState::closing) { throw std::runtime_error("missing closing state"); }
    delayed.deallocate(retained, 64, 64);
    if (!delayed.try_close().closed()) { throw std::runtime_error("delayed close failed"); }
    void* scene_data = allocate(scene, 32);
    scene.deallocate(scene_data, 32, 64);
    if (!primary.try_close().closed()) { throw std::runtime_error("primary close failed"); }
}
} // namespace

int main(int argc, char** argv)
{
    const bool capture = argc == 2 && std::string_view{argv[1]} == "--capture";
    if (argc != 1 && !capture) { return 2; }
    DK_PROFILE_THREAD_NAME("dk-memory-main");
    try {
        if (capture && (!dk::profiling::enabled() || !wait_connection(true))) { return 3; }
        workload();
        if (capture && !wait_connection(false)) { return 4; }
        std::cout << "memory=" << (dk::profiling::memory_enabled() ? "on" : "off") << ";allocations=36\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

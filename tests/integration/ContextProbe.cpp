#include <dk/memory/Buffer.hpp>
#include <dk/memory/Context.hpp>
#include <dk/memory/MemorySystem.hpp>
#include <dk/memory/ObjectPool.hpp>
#include <dk/memory/SmartPtr.hpp>
#include <dk/profiling/Memory.hpp>
#include <chrono>
#include <cstring>
#include <future>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {
using namespace dk::memory;
void require(bool value) { if (!value) { throw std::runtime_error("context probe check failed"); } }
bool wait_connection(bool desired)
{
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds{12};
    while (dk::profiling::is_connected() != desired) {
        if (std::chrono::steady_clock::now() >= end) { return false; }
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
    }
    return true;
}
MemorySystem system()
{ auto value = MemorySystem::create(); require(value.has_value()); return std::move(*value); }
ResourceHandle heap(MemorySystem& owner, DomainCategory category)
{ auto value = owner.create_heap({"context probe", category}); require(value.has_value()); return *value; }
ThreadContextOptions locals(ResourceHandle upstream) { return {upstream, {1024, 1024}, upstream, {16, 256}}; }
struct Measurement { std::uint64_t allocations = 0, assets = 0, scene = 0, jobs = 0; };
Measurement workload()
{
    DK_PROFILE_ZONE("Context.Probe");
    auto a = system(); auto b = system();
    auto assets = heap(a, DomainCategory::assets); auto a_jobs = heap(a, DomainCategory::jobs);
    auto scene = heap(b, DomainCategory::scene); auto b_jobs = heap(b, DomainCategory::jobs);
    RoutingToken ta, tb;
    {
        ThreadContext submitter{a}; ExecutionScope execution{submitter, assets};
        ta = RoutingToken::capture(); tb = RoutingToken::from_resource(scene);
    }
    Buffer a_result, b_result; std::shared_ptr<int> a_shared; std::weak_ptr<int> a_weak;
    std::promise<void> ready, resume; auto ready_future = ready.get_future(); auto resume_future = resume.get_future();
    std::exception_ptr failure;
    std::jthread worker{[&] {
        DK_PROFILE_THREAD_NAME("dk-context-worker");
        DK_PROFILE_ZONE("Context.Worker");
        ThreadContextCache cache;
        auto execute = [&](const RoutingToken& token, ResourceHandle local, std::size_t bytes) {
            DK_PROFILE_ZONE("Context.Task");
            ExecutionScope execution{cache.acquire(token, locals(local)), token}; ScratchScope scratch;
            auto temporary = scratch_vector<int>(); temporary.resize(32, 17);
            ObjectPool<int> pool{current_local_pool()};
            {
                auto value = pool.make(23);
                require(current_local_pool().try_sample().has_value()); require(*value == 23);
            }
            require(current_local_pool().try_sample().has_value()); // Observe idle backing before retirement releases it.
            auto result = try_allocate(current_resource(), bytes); require(result.has_value());
            std::memset(result->data(), temporary[0], bytes);
            return std::move(*result);
        };
        try {
            a_result = execute(ta, a_jobs, 64);
            {
                ExecutionScope execution{cache.acquire(ta, locals(a_jobs)), ta};
                a_shared = make_shared<int>(42); a_weak = a_shared;
            }
            b_result = execute(tb, b_jobs, 96);
            require(!try_current_resource()); require(cache.size() == 2);
        } catch (...) { failure = std::current_exception(); }
        ready.set_value(); resume_future.wait();
        if (failure) { return; }
        try {
            const auto retired = cache.try_retire_closed(); require(retired.has_value());
            require(retired->retired == 1 && retired->busy == 0 && cache.size() == 1);
            require(!ta.try_validate());
            auto later = execute(tb, b_jobs, 80); require(later.data()[0] == std::byte{17});
            require(!try_current_resource());
        } catch (...) { failure = std::current_exception(); }
        // Owner-thread cache destruction releases B's local backing and context lease.
    }};
    ready_future.wait();
    a.begin_close(); const auto busy_context = a.try_close();
    resume.set_value(); worker.join();
    if (failure) { std::rethrow_exception(failure); }
    require(!busy_context.closed() && busy_context.active_contexts == 1);
    require(a_jobs.snapshot().backing_requested_bytes == 0 && b_jobs.snapshot().backing_requested_bytes == 0);
    require(!try_current_resource());
    const auto busy_result = a.try_close(); require(!busy_result.closed() && busy_result.active_contexts == 0);
    require(*a_shared == 42 && a_result.data()[0] == std::byte{17});
    {
        DK_PROFILE_ZONE("Context.DelayedFree");
        a_result = {}; a_shared.reset(); require(a_weak.expired());
        require(!a.try_close().closed()); a_weak.reset(); require(a.try_close().closed());
    }
    require(b.state() == ResourceState::open && b_result.data()[0] == std::byte{17});
    require(b_result.try_resize(128).has_value()); b_result = {}; require(b.try_close().closed());
    Measurement m;
    m.assets = assets.snapshot().allocation_count; m.scene = scene.snapshot().allocation_count;
    m.jobs = a_jobs.snapshot().allocation_count + b_jobs.snapshot().allocation_count;
    m.allocations = m.assets + m.scene + m.jobs;
    return m;
}
}
int main(int argc, char** argv)
{
    const bool capture = argc == 2 && std::string_view{argv[1]} == "--capture";
    if (argc != 1 && !capture) { return 2; }
    DK_PROFILE_THREAD_NAME("dk-context-main");
    try {
        if (capture && (!dk::profiling::enabled() || !wait_connection(true))) { return 3; }
        const auto m = workload();
        if (capture && !wait_connection(false)) { return 4; }
        std::cout << "{\"status\":\"passed\",\"memory_enabled\":" << (dk::profiling::memory_enabled() ? "true" : "false")
            << ",\"systems\":2,\"tasks\":3,\"backing_allocations\":" << m.allocations
            << ",\"assets_allocations\":" << m.assets << ",\"scene_allocations\":" << m.scene
            << ",\"jobs_allocations\":" << m.jobs << "}\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}

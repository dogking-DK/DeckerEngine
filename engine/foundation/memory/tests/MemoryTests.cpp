#include <dk/memory/MemorySystem.hpp>
#include "MemoryInternal.hpp"
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <barrier>
#include <cstring>
#include <latch>
#include <limits>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace dk::memory;
namespace {
MemorySystem make_system()
{
    auto result = MemorySystem::create(); REQUIRE(result);
    return std::move(*result);
}
ResourceHandle heap(MemorySystem& system, HeapOptions options = {})
{
    auto result = system.create_heap(options); REQUIRE(result);
    return *result;
}
struct Block {
    ResourceHandle owner;
    void* pointer;
    std::size_t bytes, alignment;
    ~Block() { reset(); }
    void reset() { owner.deallocate(std::exchange(pointer, nullptr), bytes, alignment); }
};
Block block(ResourceHandle owner, std::size_t bytes, std::size_t alignment = alignof(std::max_align_t))
{
    auto result = owner.try_allocate(bytes, alignment); REQUIRE(result);
    return {std::move(owner), *result, bytes, alignment};
}

struct Controlled {
    detail::Backend real = detail::mimalloc_backend();
    bool fail_create = false;
    std::atomic<bool> fail_allocate{false}, hold_allocate{false}, hold_free{false}, hold_destroy{false};
    std::latch entered{1}, resume{1};
    std::atomic<int> destroyed{0}, alloc_events{0}, free_events{0}, attempts{0};
    detail::Backend backend()
    {
        return {this,
            [](void* c) noexcept -> void* {
                auto& s = *static_cast<Controlled*>(c);
                return s.fail_create ? nullptr : s.real.create(s.real.context);
            },
            [](void* c, void* h) noexcept {
                auto& s = *static_cast<Controlled*>(c);
                if (s.hold_destroy) { s.entered.count_down(); s.resume.wait(); }
                ++s.destroyed; s.real.destroy(s.real.context, h);
            },
            [](void* c, void* h, std::size_t n, std::size_t a) noexcept -> void* {
                auto& s = *static_cast<Controlled*>(c); ++s.attempts;
                if (s.hold_allocate) { s.entered.count_down(); s.resume.wait(); }
                return s.fail_allocate ? nullptr : s.real.allocate(s.real.context, h, n, a);
            },
            [](void* c, void* h, void* p, std::size_t n, std::size_t a) noexcept {
                auto& s = *static_cast<Controlled*>(c);
                if (s.hold_free) { s.entered.count_down(); s.resume.wait(); }
                s.real.free(s.real.context, h, p, n, a);
            }};
    }
    detail::EventSink sink()
    {
        return {this,
            [](void* c, const void*, std::size_t, DomainCategory) noexcept { ++static_cast<Controlled*>(c)->alloc_events; },
            [](void* c, const void*, DomainCategory) noexcept { ++static_cast<Controlled*>(c)->free_events; }};
    }
    MemorySystem system()
    {
        auto result = detail::MemoryAccess::create(backend(), sink()); REQUIRE(result);
        return std::move(*result);
    }
};
} // namespace

TEST_CASE("domain identity names and handles survive registry growth")
{
    auto a = make_system(); auto b = make_system();
    std::string name = "assets";
    auto first = heap(a, {name, DomainCategory::assets, 64}); name = "changed";
    auto copy = first;
    auto other = heap(b, {"assets", DomainCategory::assets, 64});
    for (int i = 0; i < 80; ++i) { const auto extra = heap(a); CHECK(extra.domain_id() != first.domain_id()); }
    CHECK(first == copy); CHECK(first != other);
    CHECK(first.name() == "assets"); CHECK(first.category() == DomainCategory::assets);
    CHECK(first.system_id() == a.id()); CHECK(a.id() != b.id());
    auto memory = block(first, 32, 32); std::memset(memory.pointer, 42, 32);
    CHECK(first.snapshot().backing_requested_bytes == 32);
    CHECK(other.snapshot().backing_requested_bytes == 0);
}

TEST_CASE("aligned heap requests and zero byte normalization are accounted")
{
    auto system = make_system(); auto resource = heap(system);
    for (std::size_t alignment = 1; alignment <= 4096; alignment *= 2) {
        auto memory = block(resource, 513, alignment);
        CHECK(reinterpret_cast<std::uintptr_t>(memory.pointer) % alignment == 0);
        std::memset(memory.pointer, 0x5a, 513);
        CHECK(resource.snapshot().backing_requested_bytes == 513);
    }
    auto limited = heap(system, {"zero", DomainCategory::general, 1});
    auto zero = block(limited, 0, 64);
    CHECK(zero.pointer != nullptr); CHECK(reinterpret_cast<std::uintptr_t>(zero.pointer) % 64 == 0);
    CHECK(limited.snapshot().reserved_bytes == 1);
    CHECK(limited.try_allocate(0).error() == AllocationError::limit_exceeded);
    zero.reset(); CHECK(limited.snapshot().live_allocations == 0);
    CHECK(resource.snapshot().backing_requested_bytes == 0);
    CHECK(resource.snapshot().peak_backing_bytes == 513);
    CHECK(resource.snapshot().allocation_count == 13);
}

TEST_CASE("invalid sizes alignments and empty handles never reach the backend")
{
    Controlled controlled; auto system = controlled.system(); auto resource = heap(system);
    CHECK(resource.try_allocate(1, 0).error() == AllocationError::invalid_alignment);
    CHECK(resource.try_allocate(1, 3).error() == AllocationError::invalid_alignment);
    const auto maximum = (std::numeric_limits<std::size_t>::max)();
    CHECK(resource.try_allocate(maximum, 1).error() == AllocationError::size_overflow);
    CHECK(resource.try_allocate(1, std::size_t{1} << (sizeof(std::size_t) * 8 - 1)).error() == AllocationError::size_overflow);
    CHECK(checked_byte_size(maximum, 2).error() == AllocationError::size_overflow);
    CHECK(*checked_byte_size(maximum, 0) == 0);
    CHECK(*checked_byte_size(7, 13) == 91);
    ResourceHandle empty;
    CHECK(empty.try_allocate(4).error() == AllocationError::invalid_handle);
    empty.deallocate(nullptr, maximum, 0); CHECK(empty.try_close().closed());
    CHECK(system.create_heap({"bad", static_cast<DomainCategory>(-1)}).error() == AllocationError::invalid_argument);
    CHECK(resource.snapshot().failure_count == 4); CHECK(controlled.attempts == 0);
    CHECK(controlled.alloc_events == 0); CHECK(resource.snapshot().reserved_bytes == 0);
}

TEST_CASE("budget failure preserves live data and released capacity is reusable")
{
    auto system = make_system(); auto resource = heap(system, {"budget", DomainCategory::assets, 64});
    auto first = block(resource, 32); auto second = block(resource, 32);
    std::memset(first.pointer, 0x7b, 32);
    CHECK(resource.try_allocate(1).error() == AllocationError::limit_exceeded);
    CHECK(resource.snapshot().reserved_bytes == 64); CHECK(resource.snapshot().allocation_count == 2);
    second.reset(); auto replacement = block(resource, 32);
    CHECK(static_cast<unsigned char*>(first.pointer)[31] == 0x7b);
    CHECK(resource.snapshot().peak_backing_bytes == 64); CHECK(resource.snapshot().failure_count == 1);
}

TEST_CASE("backend failures roll back reservation and do not publish domains or events")
{
    Controlled controlled; auto system = controlled.system();
    controlled.fail_create = true;
    CHECK(system.create_heap().error() == AllocationError::out_of_memory);
    controlled.fail_create = false; auto resource = heap(system, {"fault", DomainCategory::jobs, 64});
    CHECK(resource.domain_id() == 1); CHECK(controlled.destroyed == 0);
    controlled.fail_allocate = true;
    CHECK(resource.try_allocate(64).error() == AllocationError::out_of_memory);
    auto failed = resource.snapshot();
    CHECK(failed.reserved_bytes == 0); CHECK(failed.backing_requested_bytes == 0);
    CHECK(failed.peak_backing_bytes == 0); CHECK(failed.allocation_count == 0);
    CHECK(failed.failure_count == 1); CHECK(controlled.alloc_events == 0); CHECK(controlled.free_events == 0);
    controlled.fail_allocate = false;
    auto memory = block(resource, 64); CHECK(controlled.alloc_events == 1);
    memory.reset(); CHECK(controlled.free_events == 1);
    CHECK(system.try_close().closed()); CHECK(controlled.destroyed == 1);
    CHECK(system.try_close().closed()); CHECK(controlled.destroyed == 1);
}

TEST_CASE("closing drains live heaps and cannot reopen while another system stays usable")
{
    auto system = make_system(); auto other = make_system();
    auto resource = heap(system); auto empty = heap(system); auto independent = heap(other);
    auto memory = block(resource, 48); system.begin_close();
    CHECK(system.create_heap().error() == AllocationError::closing);
    CHECK(resource.try_allocate(1).error() == AllocationError::closing);
    const auto result = system.try_close();
    CHECK_FALSE(result.closed()); CHECK(result.live_allocations == 1); CHECK(result.backing_requested_bytes == 48);
    CHECK(empty.snapshot().state == ResourceState::closed);
    auto still_works = block(independent, 32);
    memory.reset(); CHECK(system.try_close().closed());
    CHECK(resource.snapshot().state == ResourceState::closed);
    CHECK(resource.try_allocate(1).error() == AllocationError::closing);
    CHECK(bool(resource)); CHECK(resource.name() == "general");
}

TEST_CASE("a heap can close independently from its system")
{
    auto system = make_system(); auto first = heap(system); auto second = heap(system);
    auto memory = block(first, 12);
    CHECK_FALSE(first.try_close().closed()); CHECK(system.state() == ResourceState::open);
    auto other = block(second, 12); const auto third = heap(system);
    CHECK(third.snapshot().state == ResourceState::open);
    memory.reset(); CHECK(first.try_close().closed());
}

TEST_CASE("handles keep heaps alive after system or allocating thread exits")
{
    Controlled controlled; ResourceHandle resource; void* pointer = nullptr;
    std::jthread producer{[&] {
        auto result = detail::MemoryAccess::create(controlled.backend(), controlled.sink());
        if (!result) { return; }
        auto domain = result->create_heap(); if (!domain) { return; }
        resource = *domain;
        auto bytes = resource.try_allocate(64, 64);
        if (bytes) { pointer = *bytes; std::memset(pointer, 0x24, 64); }
    }};
    producer.join();
    REQUIRE(resource); REQUIRE(pointer);
    CHECK(controlled.destroyed == 0); CHECK(resource.snapshot().state == ResourceState::closing);
    CHECK(static_cast<unsigned char*>(pointer)[63] == 0x24);
    CHECK(resource.try_allocate(1).error() == AllocationError::closing);
    resource.deallocate(pointer, 64, 64);
    resource = {}; CHECK(controlled.destroyed == 1);
}

TEST_CASE("move assignment begins closing the displaced system")
{
    auto first = make_system(); auto old = heap(first); auto live = block(old, 24);
    auto second = make_system(); const auto identity = second.id();
    first = std::move(second);
    CHECK(first.id() == identity); CHECK(second.id() == 0); CHECK(second.try_close().closed());
    CHECK(old.snapshot().state == ResourceState::closing);
    CHECK(old.try_allocate(1).error() == AllocationError::closing);
    live.reset(); CHECK(old.try_close().closed());
}

TEST_CASE("one real mimalloc heap supports concurrent allocations and foreign frees")
{
    auto system = make_system(); auto resource = heap(system);
    std::array<std::array<void*, 128>, 4> pointers{};
    std::atomic<bool> good{true}; std::barrier start{5};
    std::vector<std::jthread> workers;
    for (std::size_t t = 0; t < pointers.size(); ++t) {
        workers.emplace_back([&, t, owner = resource] {
            start.arrive_and_wait();
            for (auto& pointer : pointers[t]) {
                auto result = owner.try_allocate(128, 64);
                if (!result) { good = false; continue; }
                pointer = *result; std::memset(pointer, static_cast<int>(t + 1), 128);
            }
        });
    }
    start.arrive_and_wait(); workers.clear();
    const auto snapshot = resource.snapshot();
    std::jthread consumer{[&] {
        for (std::size_t t = 0; t < pointers.size(); ++t) {
            for (void* pointer : pointers[t]) {
                if (pointer && static_cast<unsigned char*>(pointer)[127] != t + 1) { good = false; }
                resource.deallocate(pointer, 128, 64);
            }
        }
    }};
    consumer.join();
    CHECK(good); CHECK(snapshot.live_allocations == 512); CHECK(snapshot.backing_requested_bytes == 65536);
    CHECK(resource.snapshot().reserved_bytes == 0); CHECK(system.try_close().closed());
}

TEST_CASE("concurrent reservations never exceed a domain hard budget")
{
    auto system = make_system(); auto resource = heap(system, {"bounded", DomainCategory::assets, 64});
    std::barrier phase{5}; std::vector<std::jthread> workers;
    std::atomic<int> accepted{0}, rejected{0};
    for (int i = 0; i < 4; ++i) {
        workers.emplace_back([owner = resource, &phase, &accepted, &rejected] {
            phase.arrive_and_wait();
            auto result = owner.try_allocate(32, 32);
            if (result) { ++accepted; }
            else if (result.error() == AllocationError::limit_exceeded) { ++rejected; }
            phase.arrive_and_wait(); phase.arrive_and_wait();
            if (result) { owner.deallocate(*result, 32, 32); }
        });
    }
    phase.arrive_and_wait(); phase.arrive_and_wait();
    const auto snapshot = resource.snapshot();
    phase.arrive_and_wait(); workers.clear();
    CHECK(accepted == 2); CHECK(rejected == 2); CHECK(snapshot.reserved_bytes == 64);
    CHECK(snapshot.live_allocations == 2); CHECK(resource.snapshot().reserved_bytes == 0);
}

TEST_CASE("closing waits for an admitted allocation and then its live result")
{
    Controlled controlled; controlled.hold_allocate = true;
    auto system = controlled.system(); auto resource = heap(system, {"race", DomainCategory::general, 64});
    void* pointer = nullptr;
    std::jthread worker{[&] { auto result = resource.try_allocate(64); if (result) { pointer = *result; } }};
    controlled.entered.wait(); system.begin_close();
    const auto during = system.try_close();
    const auto rejected = resource.try_allocate(1);
    const auto destroyed_during = controlled.destroyed.load();
    controlled.resume.count_down(); worker.join();
    REQUIRE(pointer);
    const auto live = system.try_close();
    resource.deallocate(pointer, 64);
    CHECK_FALSE(during.closed()); CHECK(during.active_operations == 1); CHECK(destroyed_during == 0);
    CHECK_FALSE(rejected); CHECK(rejected.error() == AllocationError::closing);
    CHECK_FALSE(live.closed()); CHECK(live.live_allocations == 1);
    CHECK(system.try_close().closed()); CHECK(controlled.destroyed == 1);
}

TEST_CASE("close cannot delete a heap while free is returning storage")
{
    Controlled controlled; auto system = controlled.system(); auto resource = heap(system);
    auto allocation = resource.try_allocate(32); REQUIRE(allocation);
    controlled.hold_free = true;
    std::jthread worker{[&] { resource.deallocate(*allocation, 32); }};
    controlled.entered.wait();
    const auto during = system.try_close(); const auto destroyed_during = controlled.destroyed.load();
    const auto free_event_before_backend = controlled.free_events.load();
    controlled.resume.count_down(); worker.join();
    CHECK_FALSE(during.closed()); CHECK(during.active_operations == 1); CHECK(destroyed_during == 0);
    CHECK(free_event_before_backend == 1); CHECK(system.try_close().closed()); CHECK(controlled.destroyed == 1);
}

TEST_CASE("concurrent resource and system close delete the backend exactly once")
{
    Controlled controlled; controlled.hold_destroy = true;
    auto system = controlled.system(); auto resource = heap(system);
    CloseResult resource_result;
    std::jthread closer{[&] { resource_result = resource.try_close(); }};
    controlled.entered.wait();
    const auto competing = system.try_close();
    const auto denied = resource.try_allocate(1);
    controlled.resume.count_down(); closer.join();
    CHECK_FALSE(competing.closed()); CHECK(competing.active_operations == 1);
    CHECK_FALSE(denied); CHECK(denied.error() == AllocationError::closing);
    CHECK(resource_result.closed()); CHECK(system.try_close().closed()); CHECK(controlled.destroyed == 1);
}

TEST_CASE("tracing pairs categories and records free before deterministic address reuse")
{
    struct Reused {
        alignas(64) std::array<std::byte, 64> storage{};
        bool allocated = false, event_live = false, ordered = true;
        int allocations = 0, frees = 0, destroys = 0;
    } reused;
    detail::Backend backend{&reused,
        [](void* c) noexcept -> void* { return c; },
        [](void* c, void*) noexcept { auto& s = *static_cast<Reused*>(c); s.ordered &= !s.allocated && !s.event_live; ++s.destroys; },
        [](void* c, void*, std::size_t, std::size_t) noexcept -> void* {
            auto& s = *static_cast<Reused*>(c); s.ordered &= !s.allocated && !s.event_live;
            s.allocated = true; return s.storage.data();
        },
        [](void* c, void*, void*, std::size_t, std::size_t) noexcept {
            auto& s = *static_cast<Reused*>(c); s.ordered &= s.allocated && !s.event_live; s.allocated = false;
        }};
    detail::EventSink sink{&reused,
        [](void* c, const void* p, std::size_t n, DomainCategory category) noexcept {
            auto& s = *static_cast<Reused*>(c);
            s.ordered &= s.allocated && !s.event_live && p == s.storage.data() && n == 64 && category == DomainCategory::assets;
            s.event_live = true; ++s.allocations;
        },
        [](void* c, const void* p, DomainCategory category) noexcept {
            auto& s = *static_cast<Reused*>(c);
            s.ordered &= s.event_live && s.allocated && p == s.storage.data() && category == DomainCategory::assets;
            s.event_live = false; ++s.frees;
        }};
    auto system = detail::MemoryAccess::create(backend, sink); REQUIRE(system);
    auto resource = heap(*system, {"asset", DomainCategory::assets});
    for (int i = 0; i < 16; ++i) {
        auto result = resource.try_allocate(64, 64); REQUIRE(result);
        CHECK(reused.event_live);
        std::jthread foreign_free{[&, pointer = *result] { resource.deallocate(pointer, 64, 64); }};
        foreign_free.join();
    }
    CHECK(system->try_close().closed()); CHECK(reused.ordered);
    CHECK(reused.allocations == 16); CHECK(reused.frees == 16); CHECK(reused.destroys == 1);
}

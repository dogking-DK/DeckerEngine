#include <dk/memory/ObjectPool.hpp>
#include <dk/memory/MemorySystem.hpp>
#include "MemoryInternal.hpp"
#include <catch2/catch_test_macros.hpp>
#include <array>
#include <atomic>
#include <barrier>
#include <cstring>
#include <limits>
#include <semaphore>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace dk::memory;
namespace {
MemorySystem system()
{ auto result = MemorySystem::create(); REQUIRE(result); return std::move(*result); }
ResourceHandle heap(MemorySystem& owner, std::size_t budget = 0)
{ auto result = owner.create_heap({"pool", DomainCategory::jobs, budget}); REQUIRE(result); return *result; }
SharedPoolResource shared(ResourceHandle upstream)
{ auto result = SharedPoolResource::create(upstream, {16, 256}); REQUIRE(result); return *result; }
struct Observed {
    detail::Backend real = detail::mimalloc_backend();
    std::atomic<bool> fail{false}, block_allocate{false}, block_free{false};
    std::atomic<std::size_t> allocations{0}, frees{0};
    std::binary_semaphore reached{0}, resume{0};
    MemorySystem make()
    {
        detail::Backend backend{this,
            [](void* p) noexcept { auto& s = *static_cast<Observed*>(p); return s.real.create(s.real.context); },
            [](void* p, void* h) noexcept { auto& s = *static_cast<Observed*>(p); s.real.destroy(s.real.context, h); },
            [](void* p, void* h, std::size_t n, std::size_t a) noexcept -> void* {
                auto& s = *static_cast<Observed*>(p);
                if (s.block_allocate.exchange(false)) { s.reached.release(); s.resume.acquire(); }
                return s.fail ? nullptr : s.real.allocate(s.real.context, h, n, a);
            },
            [](void* p, void* h, void* block, std::size_t n, std::size_t a) noexcept {
                auto& s = *static_cast<Observed*>(p);
                if (s.block_free.exchange(false)) { s.reached.release(); s.resume.acquire(); }
                s.real.free(s.real.context, h, block, n, a);
            }};
        detail::EventSink sink{this,
            [](void* p, const void*, std::size_t, DomainCategory) noexcept { ++static_cast<Observed*>(p)->allocations; },
            [](void* p, const void*, DomainCategory) noexcept { ++static_cast<Observed*>(p)->frees; }};
        auto result = detail::MemoryAccess::create(backend, sink); REQUIRE(result); return std::move(*result);
    }
};
struct alignas(256) Aligned { int value = 0; std::byte padding[256 - sizeof(int)]{}; explicit Aligned(int n) : value(n) {} };
struct Throws { Throws() { throw std::runtime_error("constructor failed"); } };
}

TEST_CASE("pool local PMR identity zero alignment and normalized counters")
{
    auto owner = system(); auto upstream = heap(owner); LocalPoolResource a{upstream}, b{upstream};
    CHECK(a.upstream() == upstream); CHECK(a.options().largest_required_pool_block > 0);
    CHECK(a.pmr_resource()->is_equal(*a.pmr_resource())); CHECK_FALSE(a.pmr_resource()->is_equal(*b.pmr_resource()));
    for (std::size_t align = 1; align <= 4096; align *= 2) {
        auto result = a.try_allocate(0, align); REQUIRE(result);
        CHECK(reinterpret_cast<std::uintptr_t>(*result) % align == 0);
        CHECK(a.snapshot().logical_live_bytes == 1); CHECK(a.snapshot().live_allocations == 1);
        CHECK(a.snapshot().idle_backing_bytes == 0); a.deallocate(*result, 0, align);
    }
    { std::pmr::vector<Aligned> objects{a.pmr_resource()}; objects.emplace_back(7); CHECK(objects.front().value == 7); }
    CHECK(a.snapshot().logical_live_bytes == 0); REQUIRE(a.try_trim()); CHECK(a.snapshot().backing_bytes == 0);
}

TEST_CASE("pool validates setup sizes alignment and empty shared handles")
{
    CHECK_THROWS_AS(LocalPoolResource{ResourceHandle{}}, PoolError);
    CHECK(SharedPoolResource::create({}).error() == AllocationError::invalid_handle);
    SharedPoolResource empty;
    CHECK(empty.state() == ResourceState::closed); CHECK(empty.try_close().closed());
    CHECK(empty.try_allocate(8).error() == AllocationError::invalid_handle);
    CHECK(empty.try_trim().error() == AllocationError::invalid_handle);
    CHECK_THROWS_AS(empty.pmr_resource()->allocate(8), std::bad_alloc);
    auto owner = system(); auto upstream = heap(owner); auto pool = shared(upstream); auto copy = pool;
    CHECK(copy == pool); CHECK(copy.pmr_resource() == pool.pmr_resource());
    CHECK(pool.try_allocate(8, 0).error() == AllocationError::invalid_alignment);
    CHECK(pool.try_allocate(8, 3).error() == AllocationError::invalid_alignment);
    CHECK(pool.try_allocate((std::numeric_limits<std::size_t>::max)()).error() == AllocationError::size_overflow);
    CHECK(pool.try_allocate(1, std::size_t{1} << (sizeof(std::size_t) * 8 - 1)).error() == AllocationError::size_overflow);
    CHECK(pool.snapshot().allocation_count == 0); CHECK(pool.snapshot().failure_count == 4);
    upstream.begin_close(); CHECK(SharedPoolResource::create(upstream).error() == AllocationError::closing);
}

TEST_CASE("pool repeated local objects reuse backing and trim preserves pool usability")
{
    Observed events; auto owner = events.make(); auto upstream = heap(owner); LocalPoolResource pool{upstream, {16, 256}};
    ObjectPool<Aligned> objects{pool};
    { auto value = objects.make(9); CHECK(value->value == 9); }
    const auto warm = pool.snapshot().backing_allocation_count;
    for (int i = 0; i < 100; ++i) { auto value = objects.make(i); CHECK(value->value == i); }
    CHECK(pool.snapshot().backing_allocation_count == warm);
    CHECK(pool.snapshot().allocation_count == 101); CHECK(events.allocations == warm);
    CHECK(pool.snapshot().idle_backing_bytes == pool.snapshot().backing_bytes);
    REQUIRE(pool.try_trim()); CHECK(pool.state() == ResourceState::open); CHECK(pool.snapshot().backing_bytes == 0);
    CHECK(events.allocations == events.frees);
    { auto value = objects.make(17); CHECK(value->value == 17); }
    REQUIRE(pool.try_trim()); CHECK(events.allocations == events.frees);
}

TEST_CASE("pool large requests and live objects prevent destructive trim")
{
    auto owner = system(); auto upstream = heap(owner); auto pool = shared(upstream);
    const auto bytes = pool.options().largest_required_pool_block * 4;
    auto block = pool.try_allocate(bytes, 256); REQUIRE(block); std::memset(*block, 0x35, bytes);
    const auto before = pool.snapshot();
    CHECK(pool.try_trim().error() == AllocationError::busy);
    CHECK(pool.snapshot().backing_bytes == before.backing_bytes); CHECK(pool.state() == ResourceState::open);
    CHECK(static_cast<unsigned char*>(*block)[bytes - 1] == 0x35);
    pool.deallocate(*block, bytes, 256); REQUIRE(pool.try_trim());
    CHECK(pool.snapshot().backing_bytes == 0); CHECK(pool.snapshot().peak_logical_bytes == bytes);
}

TEST_CASE("pool budget and backend failures preserve live data and permit retry")
{
    Observed events; auto owner = events.make(); auto upstream = heap(owner, 65536); auto pool = shared(upstream);
    auto block = pool.try_allocate(32, 8); REQUIRE(block); std::memset(*block, 0x67, 32);
    auto before = pool.snapshot();
    events.fail = true; auto failed = pool.try_allocate(32768, 64);
    REQUIRE_FALSE(failed); CHECK(failed.error() == AllocationError::out_of_memory);
    events.fail = false;
    CHECK(pool.try_allocate(131072, 64).error() == AllocationError::limit_exceeded);
    CHECK(pool.snapshot().logical_live_bytes == before.logical_live_bytes);
    CHECK(pool.snapshot().live_allocations == before.live_allocations);
    CHECK(pool.snapshot().allocation_count == before.allocation_count);
    CHECK(upstream.snapshot().reserved_bytes <= 65536);
    for (int i = 0; i < 32; ++i) { CHECK(static_cast<unsigned char*>(*block)[i] == 0x67); }
    auto retry = pool.try_allocate(32768, 64); REQUIRE(retry);
    pool.deallocate(*retry, 32768, 64); pool.deallocate(*block, 32, 8); REQUIRE(pool.try_trim());
    CHECK(upstream.snapshot().live_allocations == 0);
    events.fail = true; CHECK(pool.try_allocate(32, 8).error() == AllocationError::out_of_memory);
    CHECK(pool.snapshot().live_allocations == 0); events.fail = false;
    auto rebuilt = pool.try_allocate(32, 8); REQUIRE(rebuilt); pool.deallocate(*rebuilt, 32, 8); REQUIRE(pool.try_trim());
}

TEST_CASE("pool local operations reject foreign threads without modifying counters")
{
    auto owner = system(); auto upstream = heap(owner); LocalPoolResource pool{upstream};
    std::array<AllocationError, 4> errors{}; bool rejected = false;
    std::jthread worker{[&] {
        errors = {pool.try_allocate(4).error(), pool.try_trim().error(), pool.try_sample().error(), pool.try_close().error()};
        try { (void)pool.snapshot(); } catch (const PoolError& error) { rejected = error.code() == AllocationError::wrong_thread; }
    }}; worker.join();
    for (const auto error : errors) { CHECK(error == AllocationError::wrong_thread); }
    CHECK(rejected); CHECK(pool.snapshot().failure_count == 0); CHECK(pool.snapshot().allocation_count == 0);
}

TEST_CASE("pool object construction rollback uses allocator aware members and supports const")
{
    auto owner = system(); auto upstream = heap(owner); LocalPoolResource local{upstream}; auto pool = shared(upstream);
    ObjectPool<Throws> local_objects{local}; SharedObjectPool<Throws> shared_objects{pool};
    CHECK_THROWS_AS(local_objects.make(), std::runtime_error);
    CHECK_THROWS_AS(shared_objects.make(), std::runtime_error);
    CHECK_THROWS_AS(shared_objects.make_shared(), std::runtime_error);
    CHECK(local.snapshot().live_allocations == 0); CHECK(pool.snapshot().live_allocations == 0);
    struct LocalAware {
        using allocator_type = std::pmr::polymorphic_allocator<std::byte>;
        std::pmr::vector<int> values;
        LocalAware(std::allocator_arg_t, allocator_type a) : values(a) { values.assign(32, 7); }
    };
    struct SharedAware {
        using allocator_type = PoolAllocator<std::byte>;
        std::vector<int, PoolAllocator<int>> values;
        SharedAware(std::allocator_arg_t, allocator_type a) : values(a) { values.assign(32, 9); }
    };
    {
        ObjectPool<const LocalAware> a{local}; auto local_value = a.make();
        CHECK(local_value->values.get_allocator().resource() == local.pmr_resource());
        SharedObjectPool<const SharedAware> b{pool}; auto unique_value = b.make(); auto shared_value = b.make_shared();
        CHECK(unique_value->values.back() == 9); CHECK(shared_value->values.get_allocator().resource() == pool);
    }
    CHECK(local.snapshot().live_allocations == 0); CHECK(pool.snapshot().live_allocations == 0);
    REQUIRE(local.try_trim()); REQUIRE(pool.try_trim());
}

TEST_CASE("pool destructor runs before slot return and cannot reenter destructive trim")
{
    auto owner = system(); auto upstream = heap(owner); LocalPoolResource local{upstream};
    bool was_busy = false;
    struct Item {
        LocalPoolResource* pool; bool* busy;
        ~Item() { auto result = pool->try_trim(); *busy = !result && result.error() == AllocationError::busy; }
    };
    ObjectPool<Item> objects{local}; { auto value = objects.make(&local, &was_busy); }
    CHECK(was_busy); CHECK(local.snapshot().live_allocations == 0); REQUIRE(local.try_trim());
}

TEST_CASE("pool owning allocator propagates through containers and remains usable after move")
{
    auto owner = system(); auto upstream = heap(owner); auto a = shared(upstream); auto b = shared(upstream);
    using Vector = std::vector<int, PoolAllocator<int>>;
    {
        Vector first(32, 7, PoolAllocator<int>{a}); Vector second(16, 9, PoolAllocator<int>{b});
        Vector copied{first}; CHECK(copied.get_allocator().resource() == a);
        second = first; CHECK(second.get_allocator().resource() == a);
        Vector cloned{first, PoolAllocator<int>{b}}; first = std::move(cloned);
        CHECK(first.get_allocator().resource() == b); cloned.push_back(5); CHECK(cloned.get_allocator().resource() == b);
        first.swap(second); CHECK(first.get_allocator().resource() == a); CHECK(second.get_allocator().resource() == b);
        PoolAllocator<std::byte> rebound{first.get_allocator()}; CHECK(rebound.resource() == a);
        CHECK_THROWS_AS(rebound.allocate((std::numeric_limits<std::size_t>::max)()), std::bad_alloc);
        PoolAllocator<int> invalid; CHECK_THROWS_AS(invalid.allocate(1), std::bad_alloc);
        CHECK_THROWS_AS(first.get_allocator().allocate((std::numeric_limits<std::size_t>::max)()), std::bad_array_new_length);
    }
    REQUIRE(a.try_trim()); REQUIRE(b.try_trim());
}

TEST_CASE("pool shared deleter and weak control retain backing after wrappers and system exit")
{
    ResourceHandle upstream;
    std::unique_ptr<Aligned, PoolDeleter<Aligned, SharedPoolResource>> unique;
    std::shared_ptr<Aligned> value; std::weak_ptr<Aligned> weak;
    {
        auto owner = system(); upstream = heap(owner); auto pool = shared(upstream);
        SharedObjectPool<Aligned> objects{pool}; unique = objects.make(42); value = objects.make_shared(17); weak = value;
    }
    CHECK(upstream.state() == ResourceState::closing); CHECK(unique->value == 42); CHECK(value->value == 17);
    std::jthread consumer{[object = std::move(unique), strong = std::move(value)]() mutable { object.reset(); strong.reset(); }};
    consumer.join(); CHECK(weak.expired()); CHECK(upstream.snapshot().live_allocations > 0);
    weak.reset(); CHECK(upstream.snapshot().live_allocations == 0); CHECK(upstream.try_close().closed());
}

TEST_CASE("pool weak control makes trim busy until its final release")
{
    auto owner = system(); auto upstream = heap(owner); auto pool = shared(upstream); SharedObjectPool<int> objects{pool};
    auto value = objects.make_shared(8); std::weak_ptr<int> weak = value; value.reset();
    CHECK(weak.expired()); CHECK(pool.try_trim().error() == AllocationError::busy);
    weak.reset(); REQUIRE(pool.try_trim()); CHECK(pool.snapshot().backing_bytes == 0);
}

TEST_CASE("pool shared concurrent requests and cross thread frees preserve all payloads")
{
    auto owner = system(); auto upstream = heap(owner); auto pool = shared(upstream);
    constexpr std::size_t threads = 4, count = 64;
    REQUIRE(pool.try_trim()); // All workers now race the lazy backend reconstruction path.
    std::array<std::array<int*, count>, threads> pointers{}; std::array<bool, threads> ok{}; std::barrier sync{threads};
    std::vector<std::jthread> workers;
    for (std::size_t t = 0; t < threads; ++t) {
        workers.emplace_back([&, t, resource = pool] {
            bool success = true;
            for (std::size_t i = 0; i < count; ++i) {
                auto result = resource.try_allocate(sizeof(int), alignof(int));
                if (!result) { success = false; continue; }
                pointers[t][i] = ::new (*result) int(static_cast<int>(t * count + i));
            }
            sync.arrive_and_wait();
            const auto other = (t + 1) % threads;
            for (std::size_t i = 0; i < count; ++i) {
                auto* pointer = pointers[other][i];
                if (!pointer) { success = false; continue; }
                success = success && *pointer == static_cast<int>(other * count + i);
                resource.deallocate(pointer, sizeof(int), alignof(int));
            }
            ok[t] = success;
        });
    }
    workers.clear();
    for (const auto value : ok) { CHECK(value); }
    CHECK(pool.snapshot().live_allocations == 0); CHECK(pool.snapshot().allocation_count == threads * count);
    REQUIRE(pool.try_trim()); CHECK(upstream.snapshot().live_allocations == 0);
}

TEST_CASE("pool close rejects cached allocations while legal frees and trim still work")
{
    auto owner = system(); auto upstream = heap(owner); auto pool = shared(upstream);
    auto first = pool.try_allocate(16, 8); REQUIRE(first);
    CHECK_FALSE(pool.try_close().closed()); CHECK(pool.try_allocate(16, 8).error() == AllocationError::closing);
    CHECK_THROWS_AS(pool.pmr_resource()->allocate(16), std::bad_alloc);
    pool.deallocate(*first, 16, 8); REQUIRE(pool.try_trim()); CHECK(pool.try_close().closed());
    CHECK(upstream.state() == ResourceState::open);
    LocalPoolResource local{upstream}; auto block = local.try_allocate(8); REQUIRE(block); local.deallocate(*block, 8);
    owner.begin_close(); CHECK(local.try_allocate(8).error() == AllocationError::closing);
    REQUIRE(local.try_trim()); auto closed = local.try_close(); REQUIRE(closed); CHECK(closed->closed()); CHECK(owner.try_close().closed());
}

TEST_CASE("pool trim and close cannot overtake a backend allocation already in flight")
{
    Observed observed; auto owner = observed.make(); auto upstream = heap(owner); auto pool = shared(upstream);
    observed.block_allocate = true;
    std::expected<void*, AllocationError> block = std::unexpected(AllocationError::out_of_memory);
    std::jthread worker{[&] { block = pool.try_allocate(1048576, 64); }};
    observed.reached.acquire();
    CHECK(pool.try_trim().error() == AllocationError::busy); CHECK(pool.try_sample().error() == AllocationError::busy);
    auto closing = pool.try_close(); CHECK_FALSE(closing.closed()); CHECK(closing.active_operations == 1);
    observed.resume.release(); worker.join(); REQUIRE(block);
    CHECK(pool.try_allocate(8).error() == AllocationError::closing);
    pool.deallocate(*block, 1048576, 64); CHECK(pool.try_close().closed());
}

TEST_CASE("pool trim and close wait until backend deallocation has finished")
{
    Observed observed; auto owner = observed.make(); auto upstream = heap(owner); auto pool = shared(upstream);
    auto block = pool.try_allocate(1048576, 64); REQUIRE(block); observed.block_free = true;
    std::jthread worker{[&] { pool.deallocate(*block, 1048576, 64); }};
    observed.reached.acquire();
    CHECK(pool.snapshot().live_allocations == 1); CHECK(pool.try_trim().error() == AllocationError::busy);
    CHECK_FALSE(pool.try_close().closed()); observed.resume.release(); worker.join();
    CHECK(pool.try_close().closed()); CHECK(pool.snapshot().backing_bytes == 0);
}

TEST_CASE("pool trim maintenance preserves a concurrent close request")
{
    Observed observed; auto owner = observed.make(); auto upstream = heap(owner); auto pool = shared(upstream);
    auto block = pool.try_allocate(16, 8); REQUIRE(block); pool.deallocate(*block, 16, 8);
    REQUIRE(pool.snapshot().backing_bytes > 0); observed.block_free = true;
    bool trimmed = false; std::jthread worker{[&] { trimmed = pool.try_trim().has_value(); }};
    observed.reached.acquire(); pool.begin_close();
    CHECK(pool.try_allocate(8).error() == AllocationError::closing); CHECK_FALSE(pool.try_close().closed());
    observed.resume.release(); worker.join(); CHECK(trimmed); CHECK(pool.state() == ResourceState::closing);
    CHECK(pool.try_close().closed());
}

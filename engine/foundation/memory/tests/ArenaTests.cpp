#include <dk/memory/Arena.hpp>
#include <dk/memory/Context.hpp>
#include <dk/memory/MemorySystem.hpp>
#include <dk/memory/SmartPtr.hpp>
#include <dk/memory/Containers.hpp>
#include "MemoryInternal.hpp"
#include <catch2/catch_test_macros.hpp>
#include <array>
#include <barrier>
#include <cstring>
#include <limits>
#include <optional>
#include <stdexcept>

using namespace dk::memory;
namespace {
MemorySystem system()
{ auto result = MemorySystem::create(); REQUIRE(result); return std::move(*result); }
ResourceHandle heap(MemorySystem& owner, std::size_t budget = 0)
{ auto result = owner.create_heap({"scratch", DomainCategory::jobs, budget}); REQUIRE(result); return *result; }
template<class F> void context_error(F&& call, ContextErrorCode code)
{
    try { call(); FAIL("expected ContextError"); }
    catch (const ContextError& error) { CHECK(error.code() == code); }
}
struct Observed {
    detail::Backend real = detail::mimalloc_backend();
    bool fail = false;
    std::size_t allocations = 0, frees = 0, bytes = 0;
    MemorySystem make()
    {
        auto backend = real;
        backend.context = this;
        backend.create = [](void* p) noexcept { auto& s = *static_cast<Observed*>(p); return s.real.create(s.real.context); };
        backend.destroy = [](void* p, void* h) noexcept { auto& s = *static_cast<Observed*>(p); s.real.destroy(s.real.context, h); };
        backend.allocate = [](void* p, void* h, std::size_t n, std::size_t a) noexcept -> void* {
            auto& s = *static_cast<Observed*>(p); return s.fail ? nullptr : s.real.allocate(s.real.context, h, n, a);
        };
        backend.free = [](void* p, void* h, void* block, std::size_t n, std::size_t a) noexcept {
            auto& s = *static_cast<Observed*>(p); s.real.free(s.real.context, h, block, n, a);
        };
        detail::EventSink sink{this,
            [](void* p, const void*, std::size_t n, DomainCategory) noexcept {
                auto& s = *static_cast<Observed*>(p); ++s.allocations; s.bytes += n;
            },
            [](void* p, const void*, DomainCategory) noexcept { ++static_cast<Observed*>(p)->frees; }};
        auto result = detail::MemoryAccess::create(backend, sink); REQUIRE(result); return std::move(*result);
    }
};
}

TEST_CASE("arena nested checkpoints restore cursors and preserve outer data across chunks")
{
    auto owner = system(); auto upstream = heap(owner); ScratchArena arena{upstream, {64, 128}};
    auto outer = arena.try_checkpoint(); REQUIRE(outer);
    auto data = arena.try_allocate(13, 1); REQUIRE(data); std::memset(*data, 0x5a, 13);
    const auto before = arena.snapshot();
    auto inner = arena.try_checkpoint(); REQUIRE(inner);
    auto local = arena.try_allocate(8, 8); REQUIRE(local);
    REQUIRE(arena.try_allocate(60, 1)); REQUIRE(arena.try_allocate(160, 64));
    CHECK(arena.snapshot().chunk_count == 3);
    REQUIRE(arena.try_rewind(*inner));
    CHECK(arena.snapshot().used_bytes == before.used_bytes);
    CHECK(arena.snapshot().requested_bytes == before.requested_bytes);
    CHECK(arena.snapshot().retained_bytes == 64);
    CHECK(arena.snapshot().backing_bytes == 128);
    for (int i = 0; i < 13; ++i) { CHECK(static_cast<unsigned char*>(*data)[i] == 0x5a); }
    auto reused = arena.try_allocate(8, 8); REQUIRE(reused); CHECK(*reused == *local);
    REQUIRE(arena.try_rewind(*outer));
    CHECK(arena.snapshot().used_bytes == 0); CHECK(arena.snapshot().retained_bytes == 128);
    REQUIRE(arena.try_reset()); CHECK(upstream.snapshot().live_allocations == 0);
}

TEST_CASE("arena tokens reject wrong identity stale generation duplicate and non LIFO rewind")
{
    auto owner = system(); auto upstream = heap(owner); ScratchArena a{upstream}, b{upstream};
    auto outer = a.try_checkpoint(); auto inner = a.try_checkpoint(); REQUIRE(outer); REQUIRE(inner);
    REQUIRE(a.try_allocate(16));
    CHECK(a.try_rewind(*outer).error() == AllocationError::invalid_checkpoint);
    CHECK(b.try_rewind(*inner).error() == AllocationError::invalid_checkpoint);
    CHECK(a.try_rewind({}).error() == AllocationError::invalid_checkpoint);
    CHECK(a.try_reset().error() == AllocationError::busy);
    CHECK(a.snapshot().used_bytes == 16); CHECK(a.snapshot().active_scopes == 2);
    REQUIRE(a.try_rewind(*inner)); REQUIRE(a.try_rewind(*outer));
    CHECK(a.try_rewind(*outer).error() == AllocationError::invalid_checkpoint);
    auto next = a.try_checkpoint(); REQUIRE(next);
    CHECK(a.try_rewind(*outer).error() == AllocationError::invalid_checkpoint);
    REQUIRE(a.try_rewind(*next)); REQUIRE(a.try_reset()); CHECK(a.snapshot().generation == 2);
    auto fresh = a.try_checkpoint(); REQUIRE(fresh);
    CHECK(a.try_rewind(*next).error() == AllocationError::invalid_checkpoint);
    REQUIRE(a.try_rewind(*fresh));
}

TEST_CASE("arena handles zero bytes padding over alignment and invalid sizes")
{
    auto owner = system(); auto upstream = heap(owner); ScratchArena arena{upstream, {16384, 0}};
    CHECK(arena.try_allocate(1).error() == AllocationError::missing_scope);
    ScratchScope scope{arena};
    auto zero = arena.try_allocate(0, 1); REQUIRE(zero); CHECK(arena.snapshot().requested_bytes == 1);
    for (std::size_t alignment = 1; alignment <= 4096; alignment *= 2) {
        auto block = arena.try_allocate(3, alignment); REQUIRE(block);
        CHECK(reinterpret_cast<std::uintptr_t>(*block) % alignment == 0);
    }
    const auto before = arena.snapshot();
    CHECK(before.used_bytes >= before.requested_bytes);
    CHECK(arena.try_allocate(1, 0).error() == AllocationError::invalid_alignment);
    CHECK(arena.try_allocate(1, 3).error() == AllocationError::invalid_alignment);
    CHECK(arena.try_allocate((std::numeric_limits<std::size_t>::max)(), 1).error() == AllocationError::size_overflow);
    CHECK(arena.try_allocate(1, std::size_t{1} << (sizeof(std::size_t) * 8 - 1)).error() == AllocationError::size_overflow);
    CHECK(arena.snapshot().used_bytes == before.used_bytes);
    CHECK(arena.snapshot().backing_bytes == before.backing_bytes);
}

TEST_CASE("arena candidate budget and backend failure leave published data untouched")
{
    Observed observed; auto owner = observed.make(); auto upstream = heap(owner, 128); ScratchArena arena{upstream, {64, 64}};
    ScratchScope scope{arena};
    auto block = arena.try_allocate(60, 1); REQUIRE(block); std::memset(*block, 9, 60);
    auto before = arena.snapshot(); observed.fail = true;
    CHECK(arena.try_allocate(32, 1).error() == AllocationError::out_of_memory);
    observed.fail = false;
    CHECK(arena.try_allocate(129, 1).error() == AllocationError::limit_exceeded);
    CHECK(arena.snapshot().used_bytes == before.used_bytes);
    CHECK(arena.snapshot().backing_bytes == before.backing_bytes);
    CHECK(arena.snapshot().allocation_count == before.allocation_count);
    CHECK(arena.snapshot().failure_count == 2);
    for (int i = 0; i < 60; ++i) { CHECK(static_cast<unsigned char*>(*block)[i] == 9); }
    CHECK(upstream.snapshot().reserved_bytes == 64);
    auto tail = arena.try_allocate(4, 1); REQUIRE(tail); CHECK(*tail == static_cast<std::byte*>(*block) + 60);
    REQUIRE(arena.try_allocate(64, 1)); CHECK(observed.allocations == 2);
}

TEST_CASE("arena bounds retained chunks reuses storage and releases oversized allocations")
{
    Observed observed; auto owner = observed.make(); auto upstream = heap(owner); ScratchArena arena{upstream, {64, 96}};
    {
        ScratchScope scope{arena};
        for (int i = 0; i < 3; ++i) { REQUIRE(arena.try_allocate(64, 16)); }
        REQUIRE(arena.try_allocate(65, 64));
        CHECK(arena.snapshot().backing_bytes == 257);
    }
    CHECK(arena.snapshot().retained_bytes == 64); CHECK(arena.snapshot().backing_bytes == 64);
    CHECK(observed.allocations == 4); CHECK(observed.frees == 3);
    { ScratchScope scope{arena}; REQUIRE(arena.try_allocate(64, 16)); CHECK(observed.allocations == 4); }
    REQUIRE(arena.try_reset()); CHECK(observed.frees == 4);
    CHECK(arena.snapshot().peak_backing_bytes == 257); CHECK(arena.snapshot().chunk_count == 0);
    ScratchArena uncached{upstream, {64, 0}};
    { ScratchScope scope{uncached}; REQUIRE(uncached.try_allocate(1)); }
    CHECK(uncached.snapshot().backing_bytes == 0);
}

TEST_CASE("arena PMR destroys objects before exception rewind and deallocation is non reclaiming")
{
    auto owner = system(); auto upstream = heap(owner); ScratchArena arena{upstream, {1024, 0}};
    int destroyed = 0; bool storage_alive = true;
    struct Item {
        ScratchArena* arena; int* destroyed; bool* alive;
        ~Item() { *alive = *alive && arena->snapshot().used_bytes > 0; ++*destroyed; }
    };
    try {
        ScratchScope scope{arena};
        std::pmr::vector<Item> items{scope.resource()}; items.reserve(3);
        for (int i = 0; i < 3; ++i) { items.emplace_back(&arena, &destroyed, &storage_alive); }
        const auto used = arena.snapshot().used_bytes;
        items.clear(); CHECK(destroyed == 3); items.shrink_to_fit();
        CHECK(arena.snapshot().used_bytes == used);
        items.emplace_back(&arena, &destroyed, &storage_alive);
        throw std::runtime_error("unwind");
    } catch (const std::runtime_error&) {}
    CHECK(destroyed == 4); CHECK(storage_alive);
    CHECK(arena.snapshot().used_bytes == 0); CHECK(upstream.snapshot().live_allocations == 0);
}

TEST_CASE("arena implicit scratch requires context configuration and scope")
{
    context_error([] { ScratchScope scope; }, ContextErrorCode::missing_context);
    context_error([] { (void)scratch_vector<int>(); }, ContextErrorCode::missing_context);
    auto owner = system(); auto upstream = heap(owner); ThreadContext plain{owner};
    ExecutionScope execution{plain, upstream};
    context_error([] { ScratchScope scope; }, ContextErrorCode::missing_scratch);
    context_error([] { (void)scratch_vector<int>(); }, ContextErrorCode::missing_scope);
    ThreadContext configured{owner, upstream, {128, 128}};
    ExecutionScope nested{configured, upstream};
    context_error([] { (void)scratch_vector<int>(); }, ContextErrorCode::missing_scope);
    { ScratchScope scope; auto values = scratch_vector<int>(); values.resize(8, 3); CHECK(values[7] == 3); }
    context_error([] { (void)scratch_vector<int>(); }, ContextErrorCode::missing_scope);
}

TEST_CASE("arena domain switches preserve scratch and persistent factories keep their domain")
{
    auto owner = system(); auto jobs = heap(owner); auto assets = heap(owner); auto scene = heap(owner);
    ThreadContext context{owner, jobs, {256, 256}}; ExecutionScope execution{context, assets};
    std::shared_ptr<int> result;
    {
        ScratchScope outer; auto values = scratch_vector<int>(); values.resize(8, 7);
        auto* resource = values.get_allocator().resource();
        {
            DomainScope domain{scene}; CHECK(current_scratch_resource() == resource);
            ScratchScope inner; auto temporary = scratch_vector<int>(); temporary.resize(8, 4);
            result = dk::memory::make_shared<int>(temporary.front());
            CHECK(scene.snapshot().live_allocations == 1); CHECK(assets.snapshot().live_allocations == 0);
        }
        CHECK(current_scratch_resource() == resource); CHECK(values[3] == 7);
        CHECK(context.scratch().upstream() == jobs);
    }
    REQUIRE(result); CHECK(*result == 4); CHECK(jobs.snapshot().live_allocations == 1);
    CHECK(context.scratch().snapshot().used_bytes == 0);
}

TEST_CASE("arena execution frames isolate nested systems and restore scratch on exceptions")
{
    auto a = system(); auto ah = heap(a); auto b = system(); auto bh = heap(b);
    ThreadContext ac{a, ah}, bc{b, bh}; ExecutionScope execution{ac, ah}; ScratchScope outer;
    auto values = scratch_vector<int>(); values.resize(4, 11); auto* original = current_scratch_resource();
    try {
        ExecutionScope nested{bc, bh};
        context_error([] { (void)current_scratch_resource(); }, ContextErrorCode::missing_scope);
        ScratchScope scope; CHECK(current_scratch_resource() != original);
        auto other = scratch_vector<int>(); other.resize(16, 2);
        throw std::runtime_error("restore");
    } catch (const std::runtime_error&) {}
    CHECK(current_scratch_resource() == original); CHECK(values[0] == 11);
    CHECK(bc.scratch().snapshot().used_bytes == 0);
    { ExecutionScope same_context{ac, ah}; context_error([] { (void)scratch_vector<int>(); }, ContextErrorCode::missing_scope); }
}

TEST_CASE("arena explicit scope does not bind TLS and persistent copy survives rewind")
{
    auto owner = system(); auto upstream = heap(owner); ScratchArena arena{upstream};
    dk::Vector<int> output{Allocator<int>{upstream}};
    {
        ScratchScope scope{arena}; context_error([] { (void)scratch_vector<int>(); }, ContextErrorCode::missing_context);
        std::pmr::vector<int> temporary{scope.resource()}; temporary.assign(32, 42);
        output.assign(temporary.begin(), temporary.end());
    }
    REQUIRE(arena.try_reset()); CHECK(output.size() == 32); CHECK(output.back() == 42);
    CHECK(output.get_allocator().resource() == upstream);
}

TEST_CASE("arena wrong thread calls reject without changing owner state")
{
    auto owner = system(); auto upstream = heap(owner); ThreadContext context{owner, upstream}; auto& arena = context.scratch();
    auto point = arena.try_checkpoint(); REQUIRE(point); REQUIRE(arena.try_allocate(32));
    std::array<AllocationError, 4> errors{}; ContextErrorCode context_code{}; bool snapshot_rejected = false;
    std::jthread worker{[&] {
        errors = {arena.try_allocate(1).error(), arena.try_checkpoint().error(), arena.try_rewind(*point).error(), arena.try_reset().error()};
        try { (void)context.scratch(); } catch (const ContextError& error) { context_code = error.code(); }
        try { (void)arena.snapshot(); } catch (const ScratchError& error) { snapshot_rejected = error.code() == AllocationError::wrong_thread; }
    }}; worker.join();
    for (auto error : errors) { CHECK(error == AllocationError::wrong_thread); }
    CHECK(context_code == ContextErrorCode::wrong_thread); CHECK(snapshot_rejected);
    CHECK(arena.snapshot().used_bytes == 32); CHECK(arena.snapshot().failure_count == 0);
    REQUIRE(arena.try_rewind(*point));
}

TEST_CASE("arena contexts on two threads use independent scratch against a shared heap")
{
    auto owner = system(); auto upstream = heap(owner); std::barrier sync{2};
    std::array<void*, 2> addresses{}; std::array<bool, 2> correct{};
    const auto work = [&](std::size_t index) {
        ThreadContext context{owner, upstream, {128, 128}}; ExecutionScope execution{context, upstream}; ScratchScope scope;
        auto data = scratch_vector<int>(); data.assign(16, static_cast<int>(index)); addresses[index] = data.data();
        sync.arrive_and_wait(); correct[index] = data.back() == static_cast<int>(index) && addresses[0] != addresses[1];
    };
    std::jthread first{work, 0}; std::jthread second{work, 1}; first.join(); second.join();
    CHECK(correct[0]); CHECK(correct[1]); CHECK(upstream.snapshot().live_allocations == 0);
    CHECK(owner.try_close().closed());
}

TEST_CASE("arena closing rejects cached bump but permits rewind reset and context cleanup")
{
    auto owner = system(); auto upstream = heap(owner);
    {
        ThreadContext context{owner, upstream, {128, 128}}; auto& arena = context.scratch();
        ExecutionScope execution{context, upstream};
        {
            ScratchScope scope; REQUIRE(arena.try_allocate(16));
            owner.begin_close(); auto busy = owner.try_close(); CHECK_FALSE(busy.closed()); CHECK(busy.active_contexts == 1);
            CHECK(arena.try_allocate(1).error() == AllocationError::closing);
            CHECK(arena.try_checkpoint().error() == AllocationError::closing);
            context_error([] { (void)scratch_vector<int>(); }, ContextErrorCode::closing);
        }
        CHECK(arena.snapshot().retained_bytes == 128);
        REQUIRE(arena.try_reset()); CHECK(upstream.snapshot().live_allocations == 0);
    }
    CHECK(owner.try_close().closed()); CHECK(upstream.state() == ResourceState::closed);
}

TEST_CASE("arena context destruction after system wrapper drops cached chunks safely")
{
    ResourceHandle upstream; std::unique_ptr<ThreadContext> context;
    {
        auto owner = system(); upstream = heap(owner); context = std::make_unique<ThreadContext>(owner, upstream);
        ExecutionScope execution{*context, upstream}; ScratchScope scope; auto data = scratch_vector<int>(); data.resize(4);
    }
    CHECK(upstream.state() == ResourceState::closing); CHECK(upstream.snapshot().live_allocations == 1);
    context.reset(); CHECK(upstream.snapshot().live_allocations == 0); CHECK(upstream.try_close().closed());
}

TEST_CASE("arena setup validation releases failed context leases and heap events count chunks only")
{
    Observed observed; auto owner = observed.make(); auto upstream = heap(owner); auto other = system(); auto foreign = heap(other);
    context_error([&] { ThreadContext invalid{owner, foreign}; }, ContextErrorCode::wrong_system);
    CHECK_THROWS_AS((ScratchArena{upstream, {0, 0}}), ScratchError);
    CHECK_THROWS_AS((ThreadContext{owner, upstream, {0, 0}}), ScratchError);
    CHECK_THROWS_AS((ScratchArena{ResourceHandle{}}), ScratchError);
    {
        ScratchArena arena{upstream, {1024, 1024}};
        { ScratchScope scope{arena}; for (int i = 0; i < 100; ++i) { REQUIRE(arena.try_allocate(8, 8)); } }
        CHECK(observed.allocations == 1); CHECK(observed.bytes == 1024); CHECK(observed.frees == 0);
        CHECK(arena.snapshot().allocation_count == 100);
    }
    CHECK(observed.frees == 1); CHECK(owner.try_close().closed());
}

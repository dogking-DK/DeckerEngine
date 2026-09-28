#include <dk/memory/Context.hpp>
#include <dk/memory/MemorySystem.hpp>
#include <dk/memory/Containers.hpp>
#include <dk/memory/SmartPtr.hpp>
#include <dk/memory/ObjectPool.hpp>
#include "MemoryInternal.hpp"
#include <catch2/catch_test_macros.hpp>
#include <atomic>
#include <barrier>
#include <latch>
#include <optional>
#include <stdexcept>

using namespace dk::memory;
namespace {
MemorySystem system()
{ auto value = MemorySystem::create(); REQUIRE(value); return std::move(*value); }
ResourceHandle heap(MemorySystem& owner)
{ auto value = owner.create_heap(); REQUIRE(value); return *value; }
ThreadContextOptions locals(ResourceHandle upstream)
{ return {upstream, {1024, 1024}, upstream, {16, 256}}; }
template<class F> void context_error(F&& call, ContextErrorCode code)
{
    try { call(); FAIL("expected ContextError"); }
    catch (const ContextError& error) { CHECK(error.code() == code); }
}
}

TEST_CASE("context token captures nested persistent domain without borrowing submitter")
{
    auto owner = system(); auto a = heap(owner); auto b = heap(owner);
    RoutingToken token;
    context_error([] { (void)RoutingToken::capture(); }, ContextErrorCode::missing_context);
    CHECK(RoutingToken{}.try_validate().error() == ContextErrorCode::invalid_token);
    context_error([] { (void)RoutingToken::from_resource({}); }, ContextErrorCode::invalid_resource);
    {
        ThreadContext submitter{owner, locals(a)};
        ExecutionScope execution{submitter, a}; ScratchScope scratch;
        DomainScope domain{b}; token = RoutingToken::capture();
        auto explicit_token = RoutingToken::from_resource(a);
        CHECK(explicit_token.resource() == a); CHECK(current_resource() == b);
    }
    REQUIRE(token.try_validate()); CHECK(token.resource() == b);
    ThreadContextCache cache;
    auto& context = cache.acquire(token, locals(a));
    {
        ExecutionScope execution{context, token};
        context_error([] { (void)current_scratch_resource(); }, ContextErrorCode::missing_scope);
        ScratchScope scratch;
        auto temporary = scratch_vector<int>(); temporary.resize(64, 7);
        CHECK(context.scratch().upstream() == a);
        CHECK(current_local_pool().upstream() == a);
        dk::Vector<int> result(8, 3); CHECK(result.get_allocator().resource() == b);
    }
    CHECK(try_current_resource().error() == ContextErrorCode::missing_context);
}

TEST_CASE("context local pool assembly validates configuration and unwinds failed leases")
{
    auto owner = system(); auto other = system(); auto a = heap(owner); auto b = heap(other);
    {
        ThreadContext minimal{owner};
        context_error([&] { (void)minimal.local_pool(); }, ContextErrorCode::missing_pool);
    }
    context_error([&] { ThreadContext bad{owner, {a, {}, b, {}}}; }, ContextErrorCode::wrong_system);
    ThreadContextCache cache; auto token = RoutingToken::from_resource(a);
    context_error([&] { (void)cache.acquire(token, {a, {}, b, {}}); }, ContextErrorCode::wrong_system);
    CHECK(cache.size() == 0);
    auto bad_options = locals(a); bad_options.scratch_options.chunk_bytes = 0;
    CHECK_THROWS_AS(cache.acquire(token, bad_options), ScratchError);
    CHECK(cache.size() == 0);
    auto& context = cache.acquire(token, locals(a));
    auto changed = locals(a); changed.local_pool_options.max_blocks_per_chunk = 8;
    context_error([&] { (void)cache.acquire(token, changed); }, ContextErrorCode::configuration_mismatch);
    CHECK(&cache.acquire(token, locals(a)) == &context);
    REQUIRE(cache.try_clear()); CHECK(cache.size() == 0);
    CHECK(owner.try_close().closed()); CHECK(other.try_close().closed());
}

TEST_CASE("context local backing failure preserves routing and can recover")
{
    std::atomic<bool> fail{false};
    auto backend = detail::mimalloc_backend(); backend.context = &fail;
    backend.allocate = [](void* state, void* h, std::size_t n, std::size_t a) noexcept -> void* {
        if (*static_cast<std::atomic<bool>*>(state)) { return nullptr; }
        auto real = detail::mimalloc_backend(); return real.allocate(real.context, h, n, a);
    };
    auto owner = detail::MemoryAccess::create(backend, {}); REQUIRE(owner);
    auto resource = heap(*owner); auto token = RoutingToken::from_resource(resource);
    ThreadContextCache cache; auto& context = cache.acquire(token, locals(resource));
    {
        ExecutionScope execution{context, token}; ScratchScope scratch;
        auto temporary = scratch_vector<int>(); // MSVC Debug's noexcept constructor allocates an iterator proxy.
        fail = true;
        CHECK_THROWS_AS(temporary.resize(4096), std::bad_alloc); // Force a new chunk, beyond the proxy's cached chunk.
        CHECK(context.local_pool().try_allocate(32).error() == AllocationError::out_of_memory);
        CHECK(current_resource() == resource);
        fail = false; temporary.resize(64, 9); CHECK(temporary[0] == 9);
        ObjectPool<int> pool{current_local_pool()}; auto value = pool.make(8); CHECK(*value == 8);
    }
    CHECK(cache.try_clear()->retired == 1); CHECK(owner->try_close().closed());
}

TEST_CASE("context cache restores nested systems domains and scratch after exceptions")
{
    auto a = system(); auto b = system(); auto ra = heap(a); auto alt = heap(a); auto rb = heap(b);
    auto ta = RoutingToken::from_resource(ra); auto tb = RoutingToken::from_resource(rb);
    ThreadContextCache cache;
    auto& ca = cache.acquire(ta, locals(ra)); auto& cb = cache.acquire(tb, locals(rb));
    {
        ExecutionScope execution{ca, ta}; ScratchScope outer;
        auto* original = current_scratch_resource();
        try {
            DomainScope domain{alt}; auto nested = RoutingToken::capture(); CHECK(nested.resource() == alt);
            ExecutionScope inner{cb, tb};
            context_error([] { (void)current_scratch_resource(); }, ContextErrorCode::missing_scope);
            ScratchScope scratch; auto temporary = scratch_vector<int>(); temporary.resize(24);
            ObjectPool<int> pool{current_local_pool()}; auto value = pool.make(3);
            CHECK(current_resource() == rb); CHECK(*value == 3);
            throw std::runtime_error("callback failed");
        } catch (const std::runtime_error&) {}
        CHECK(current_resource() == ra); CHECK(current_scratch_resource() == original);
        CHECK(ca.scratch().snapshot().active_scopes == 1); CHECK(cb.scratch().snapshot().active_scopes == 0);
        CHECK(cb.local_pool().snapshot().live_allocations == 0);
        context_error([&] { ExecutionScope bad{ca, tb}; }, ContextErrorCode::wrong_system);
        context_error([&] { ExecutionScope bad{ca, RoutingToken{}}; }, ContextErrorCode::invalid_token);
        CHECK(current_resource() == ra);
    }
    CHECK_FALSE(try_current_resource()); CHECK(cache.try_clear()->retired == 2);
}

TEST_CASE("context retirement preserves active frames checkpoints and pool objects")
{
    auto owner = system(); auto resource = heap(owner); auto token = RoutingToken::from_resource(resource);
    ThreadContextCache cache; auto& context = cache.acquire(token, locals(resource));
    auto& arena = context.scratch(); auto& pool = context.local_pool();
    {
        ExecutionScope execution{context, token};
        CHECK(cache.try_clear()->busy == 1); CHECK(current_resource() == resource);
    }
    auto checkpoint = arena.try_checkpoint(); REQUIRE(checkpoint);
    auto temporary = arena.try_allocate(16); REQUIRE(temporary);
    CHECK(cache.try_clear()->busy == 1);
    REQUIRE(arena.try_rewind(*checkpoint));
    auto block = pool.try_allocate(32); REQUIRE(block);
    CHECK(cache.try_clear()->busy == 1);
    const auto busy = owner.try_close(); CHECK_FALSE(busy.closed()); CHECK(busy.active_contexts == 1);
    CHECK(cache.try_retire_closed()->busy == 1);
    CHECK(pool.try_allocate(32).error() == AllocationError::closing);
    context_error([&] { (void)context.local_pool(); }, ContextErrorCode::closing);
    pool.deallocate(*block, 32);
    CHECK(cache.try_retire_closed()->retired == 1); CHECK(cache.size() == 0);
    CHECK(resource.snapshot().backing_requested_bytes == 0); CHECK(owner.try_close().closed());
    CHECK_FALSE(try_current_resource());
}

TEST_CASE("context retirement clears idle systems while leaving busy systems untouched")
{
    auto a = system(); auto b = system(); auto c = system();
    auto ra = heap(a); auto rb = heap(b); auto rc = heap(c);
    auto ta = RoutingToken::from_resource(ra); auto tb = RoutingToken::from_resource(rb); auto tc = RoutingToken::from_resource(rc);
    ThreadContextCache cache;
    auto& ca = cache.acquire(ta, locals(ra)); (void)cache.acquire(tb, locals(rb)); (void)cache.acquire(tc);
    {
        ExecutionScope execution{ca, ta}; ScratchScope scratch;
        a.begin_close(); b.begin_close();
        const auto retired = cache.try_retire_closed(); REQUIRE(retired);
        CHECK(retired->retired == 1); CHECK(retired->busy == 1); CHECK(cache.size() == 2);
        CHECK(b.try_close().closed()); CHECK_FALSE(a.try_close().closed());
    }
    CHECK(cache.try_retire_closed()->retired == 1); CHECK(a.try_close().closed());
    CHECK(cache.size() == 1);
    { ExecutionScope execution{cache.acquire(tc), tc}; CHECK(current_resource() == rc); }
    CHECK(cache.try_clear()->retired == 1); CHECK(c.try_close().closed());
}

TEST_CASE("context cache clear permits fresh binding with old owning token")
{
    auto owner = system(); auto resource = heap(owner); auto token = RoutingToken::from_resource(resource);
    ThreadContextCache cache;
    for (int i = 0; i < 3; ++i) {
        auto& context = cache.acquire(token, locals(resource));
        {
            ExecutionScope execution{context, token}; ScratchScope scratch;
            auto temporary = scratch_vector<int>(); temporary.resize(64, i);
            CHECK(temporary.front() == i);
        }
        CHECK(context.scratch().snapshot().backing_bytes > 0);
        CHECK(cache.try_clear()->retired == 1); CHECK(resource.snapshot().backing_requested_bytes == 0);
        CHECK_FALSE(try_current_resource());
    }
    CHECK(owner.try_close().closed()); CHECK(token.try_validate().error() == ContextErrorCode::closing);
    context_error([&] { (void)cache.acquire(token); }, ContextErrorCode::closing);
    CHECK(cache.size() == 0);
}

TEST_CASE("context cache and local access reject foreign threads without mutation")
{
    auto owner = system(); auto resource = heap(owner); auto token = RoutingToken::from_resource(resource);
    ThreadContextCache cache; auto& context = cache.acquire(token, locals(resource));
    std::atomic<int> rejected{0};
    std::jthread worker{[&] {
        if (cache.try_clear().error() == ContextErrorCode::wrong_thread) { ++rejected; }
        if (cache.try_retire_closed().error() == ContextErrorCode::wrong_thread) { ++rejected; }
        auto check = [&](auto&& call) {
            try { call(); } catch (const ContextError& error) { if (error.code() == ContextErrorCode::wrong_thread) { ++rejected; } }
        };
        check([&] { (void)cache.acquire(token); }); check([&] { (void)cache.size(); });
        check([&] { (void)context.local_pool(); }); check([&] { ExecutionScope execution{context, token}; });
    }};
    worker.join(); CHECK(rejected == 6); CHECK(cache.size() == 1); CHECK(cache.try_clear()->retired == 1);
}

TEST_CASE("context reused worker routes captured domains and keeps results after worker exit")
{
    auto a = system(); auto b = system(); auto ra = heap(a); auto alt = heap(a); auto rb = heap(b);
    RoutingToken ta, tb, nested;
    {
        ThreadContext submitter{a}; ExecutionScope execution{submitter, ra}; ta = RoutingToken::capture();
        DomainScope domain{alt}; nested = RoutingToken::capture(); tb = RoutingToken::from_resource(rb);
    }
    std::optional<dk::Vector<int>> result; std::weak_ptr<int> weak; std::exception_ptr failure;
    std::jthread worker{[&] {
        try {
            ThreadContextCache cache;
            for (const auto& token : {ta, tb, nested, tb, ta}) {
                const auto options = locals(token.system_id() == a.id() ? ra : rb);
                {
                    ExecutionScope execution{cache.acquire(token, options), token}; ScratchScope scratch;
                    auto temporary = scratch_vector<int>(); temporary.resize(16, 6);
                    result.emplace(temporary.begin(), temporary.end());
                    if (result->get_allocator().resource() != token.resource()) { throw std::runtime_error("wrong route"); }
                    auto value = make_shared<int>(9); weak = value;
                    ObjectPool<int> pool{current_local_pool()}; auto local = pool.make(7);
                    if (*local != 7) { throw std::runtime_error("local object"); }
                }
                if (try_current_resource()) { throw std::runtime_error("stale TLS"); }
            }
            if (cache.size() != 2) { throw std::runtime_error("not reused"); }
        } catch (...) { failure = std::current_exception(); }
    }};
    worker.join(); if (failure) { std::rethrow_exception(failure); }
    REQUIRE(result); CHECK(result->front() == 6); CHECK(weak.expired());
    auto busy = a.try_close(); CHECK_FALSE(busy.closed()); CHECK(busy.active_contexts == 0);
    result.reset(); CHECK_FALSE(a.try_close().closed()); weak.reset(); CHECK(a.try_close().closed()); CHECK(b.try_close().closed());
}

TEST_CASE("context queued cancellation does not create a lease or prevent close")
{
    auto owner = system(); auto resource = heap(owner);
    std::optional<RoutingToken> queued{RoutingToken::from_resource(resource)};
    ThreadContextCache cache; queued.reset();
    CHECK(cache.size() == 0); CHECK(owner.try_close().closed());
}

TEST_CASE("context cooperative cancellation restores outer route and closing token cannot replace it")
{
    auto a = system(); auto b = system(); auto ra = heap(a); auto rb = heap(b);
    auto ta = RoutingToken::from_resource(ra); auto tb = RoutingToken::from_resource(rb);
    ThreadContextCache cache; auto& ca = cache.acquire(ta, locals(ra)); auto& cb = cache.acquire(tb, locals(rb));
    std::stop_source cancellation;
    {
        ExecutionScope outer{ca, ta}; ScratchScope outer_scratch; auto* original = current_scratch_resource();
        auto callback = [&] {
            ExecutionScope inner{cb, tb}; ScratchScope scratch;
            auto temporary = scratch_vector<int>(); temporary.resize(32);
            ObjectPool<int> pool{current_local_pool()}; auto value = pool.make(5);
            cancellation.request_stop();
            if (cancellation.stop_requested()) { return; }
            FAIL("cancelled callback continued");
        };
        callback();
        CHECK(current_resource() == ra); CHECK(current_scratch_resource() == original);
        CHECK(cb.scratch().snapshot().active_scopes == 0); CHECK(cb.local_pool().snapshot().live_allocations == 0);
        b.begin_close();
        context_error([&] { ExecutionScope denied{cb, tb}; }, ContextErrorCode::closing);
        context_error([&] { (void)cache.acquire(tb, locals(rb)); }, ContextErrorCode::closing);
        CHECK(current_resource() == ra); CHECK(current_scratch_resource() == original);
        CHECK(cache.try_retire_closed()->retired == 1); CHECK(b.try_close().closed());
    }
    CHECK_FALSE(try_current_resource());
}

TEST_CASE("context shared pool results retain backing after local cache retirement")
{
    auto owner = system(); auto resource = heap(owner); auto token = RoutingToken::from_resource(resource);
    auto shared = SharedPoolResource::create(resource); REQUIRE(shared);
    ThreadContextCache cache; std::shared_ptr<int> result; std::weak_ptr<int> weak;
    {
        ExecutionScope execution{cache.acquire(token, locals(resource)), token}; ScratchScope scratch;
        auto temporary = scratch_vector<int>(); temporary.resize(32, 11);
        SharedObjectPool<int> objects{*shared}; result = objects.make_shared(temporary[0]); weak = result;
    }
    owner.begin_close(); CHECK(cache.try_retire_closed()->retired == 1);
    CHECK(*result == 11); CHECK(owner.try_close().active_contexts == 0);
    std::jthread consumer{[value = std::move(result)]() mutable { value.reset(); }}; consumer.join();
    CHECK(weak.expired()); CHECK_FALSE(shared->try_close().closed());
    weak.reset(); CHECK(shared->try_close().closed()); CHECK(owner.try_close().closed());
}

TEST_CASE("context token and cache survive system wrapper destruction without allowing new work")
{
    ThreadContextCache cache; RoutingToken token; ResourceHandle resource;
    std::shared_ptr<int> result; std::weak_ptr<int> weak;
    {
        auto owner = system(); resource = heap(owner); token = RoutingToken::from_resource(resource);
        ExecutionScope execution{cache.acquire(token, locals(resource)), token}; ScratchScope scratch;
        result = make_shared<int>(42); weak = result;
        auto temporary = scratch_vector<int>(); temporary.resize(32);
    }
    CHECK(*result == 42); CHECK(token.try_validate().error() == ContextErrorCode::closing);
    context_error([&] { (void)cache.acquire(token, locals(resource)); }, ContextErrorCode::closing);
    CHECK(cache.try_retire_closed()->retired == 1);
    result.reset(); CHECK_FALSE(resource.try_close().closed()); weak.reset(); CHECK(resource.try_close().closed());
}

TEST_CASE("context token registration races close without admitting work after closing")
{
    for (int iteration = 0; iteration < 32; ++iteration) {
        auto owner = system(); auto resource = heap(owner); auto token = RoutingToken::from_resource(resource);
        std::barrier start{2}; std::latch attempted{1}, release{1}; std::atomic<bool> admitted{false}, rejected{false};
        std::jthread worker{[&] {
            ThreadContextCache cache; start.arrive_and_wait();
            try { (void)cache.acquire(token); admitted = true; }
            catch (const ContextError& error) { rejected = error.code() == ContextErrorCode::closing; }
            attempted.count_down(); release.wait();
        }};
        start.arrive_and_wait(); owner.begin_close(); attempted.wait();
        const auto closing = owner.try_close(); release.count_down(); worker.join();
        CHECK(admitted != rejected);
        CHECK(closing.active_contexts == (admitted ? 1 : 0));
        CHECK(closing.closed() == !admitted); CHECK(owner.try_close().closed());
    }
}

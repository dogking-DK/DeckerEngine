#include <dk/memory/MemorySystem.hpp>
#include <dk/memory/Buffer.hpp>
#include <dk/memory/Containers.hpp>
#include <dk/memory/SmartPtr.hpp>
#include "MemoryInternal.hpp"
#include <catch2/catch_test_macros.hpp>
#include <atomic>
#include <barrier>
#include <cstring>
#include <limits>
#include <optional>
#include <scoped_allocator>
#include <stdexcept>

using namespace dk::memory;
namespace {
MemorySystem system()
{
    auto result = MemorySystem::create(); REQUIRE(result); return std::move(*result);
}
ResourceHandle heap(MemorySystem& owner, std::size_t budget = 0)
{
    auto result = owner.create_heap({"ownership", DomainCategory::assets, budget}); REQUIRE(result); return *result;
}
struct alignas(256) Aligned { int value = 17; std::byte padding[256 - sizeof(int)]{}; };
struct Plain { dk::Vector<int> values; Plain() { values.resize(32, 7); } };
struct Aware {
    using allocator_type = Allocator<std::byte>;
    dk::Vector<int> values;
    Aware(std::allocator_arg_t, const allocator_type& allocator, int value)
        : values(32, value, Allocator<int>{allocator}) {}
};
struct Throws {
    dk::Vector<int> values;
    Throws() { values.resize(32); throw std::runtime_error("object construction failed"); }
};
struct Observed {
    detail::Backend real = detail::mimalloc_backend();
    std::atomic<int> destroyed{0}, allocations{0}, frees{0};
    std::atomic<bool> fail{false};
    MemorySystem make()
    {
        detail::Backend backend{this,
            [](void* p) noexcept -> void* { auto& s = *static_cast<Observed*>(p); return s.real.create(s.real.context); },
            [](void* p, void* h) noexcept { auto& s = *static_cast<Observed*>(p); ++s.destroyed; s.real.destroy(s.real.context, h); },
            [](void* p, void* h, std::size_t n, std::size_t a) noexcept -> void* {
                auto& s = *static_cast<Observed*>(p); return s.fail ? nullptr : s.real.allocate(s.real.context, h, n, a);
            },
            [](void* p, void* h, void* block, std::size_t n, std::size_t a) noexcept {
                auto& s = *static_cast<Observed*>(p); s.real.free(s.real.context, h, block, n, a);
            }};
        detail::EventSink sink{this,
            [](void* p, const void*, std::size_t, DomainCategory) noexcept { ++static_cast<Observed*>(p)->allocations; },
            [](void* p, const void*, DomainCategory) noexcept { ++static_cast<Observed*>(p)->frees; }};
        auto result = detail::MemoryAccess::create(backend, sink); REQUIRE(result); return std::move(*result);
    }
};
template<class F> void context_error(F&& f, ContextErrorCode expected)
{
    try { f(); FAIL("expected ContextError"); }
    catch (const ContextError& error) { CHECK(error.code() == expected); CHECK(std::strlen(error.what()) > 0); }
}
}

TEST_CASE("ownership PMR uses stable identity alignment and explicit clone resources")
{
    auto owner = system(); auto a = heap(owner); auto b = heap(owner); auto copy = a;
    CHECK(a.pmr_resource()->is_equal(*copy.pmr_resource()));
    CHECK_FALSE(a.pmr_resource()->is_equal(*b.pmr_resource()));
    auto* defaults = std::pmr::get_default_resource();
    {
        std::pmr::vector<Aligned> values{a.pmr_resource()}; values.resize(8);
        CHECK(reinterpret_cast<std::uintptr_t>(values.data()) % alignof(Aligned) == 0);
        std::pmr::vector<Aligned> cloned{values, b.pmr_resource()};
        CHECK(cloned[3].value == 17); CHECK(cloned.get_allocator().resource() == b.pmr_resource());
        CHECK(a.snapshot().live_allocations > 0); CHECK(b.snapshot().live_allocations > 0);
        owner.begin_close();
        CHECK_THROWS_AS(a.pmr_resource()->allocate(8), std::bad_alloc);
    }
    CHECK(owner.try_close().closed()); CHECK(std::pmr::get_default_resource() == defaults);
    ResourceHandle empty; CHECK_THROWS_AS(empty.pmr_resource()->allocate(1), std::bad_alloc);
}

TEST_CASE("ownership allocator propagation preserves owners through copy move swap and rebind")
{
    auto owner = system(); auto a = heap(owner); auto b = heap(owner);
    {
        dk::Vector<int> first(32, 1, Allocator<int>{a});
        dk::Vector<int> second(16, 2, Allocator<int>{b});
        dk::Vector<int> copied{first}; CHECK(copied.get_allocator().resource() == a);
        dk::Vector<int> cloned{first, Allocator<int>{b}}; CHECK(cloned.get_allocator().resource() == b);
        second = first; CHECK(second.get_allocator().resource() == a); CHECK(second == first);
        second = std::move(cloned); CHECK(second.get_allocator().resource() == b);
        cloned.push_back(9); CHECK(cloned.get_allocator().resource() == b); // No TLS after move.
        first.swap(second); CHECK(first.get_allocator().resource() == b); CHECK(second.get_allocator().resource() == a);
        dk::Vector<int> moved{std::move(first)}; first.push_back(8);
        CHECK(moved.get_allocator().resource() == b); CHECK(first.get_allocator().resource() == b);
        Allocator<int> ints{a}; Allocator<Aligned> aligned{ints};
        CHECK(aligned == ints); auto* p = aligned.allocate(2);
        CHECK(reinterpret_cast<std::uintptr_t>(p) % alignof(Aligned) == 0); aligned.deallocate(p, 2);
    }
    CHECK(a.snapshot().live_allocations == 0); CHECK(b.snapshot().live_allocations == 0);
}

TEST_CASE("ownership allocator failures throw standard exceptions and zero allocation is paired")
{
    auto owner = system(); auto resource = heap(owner, 32); Allocator<std::uint64_t> allocator{resource};
    CHECK_THROWS_AS(allocator.allocate((std::numeric_limits<std::size_t>::max)()), std::bad_array_new_length);
    CHECK_THROWS_AS(allocator.allocate(5), std::bad_alloc);
    CHECK(resource.snapshot().backing_requested_bytes == 0);
    auto* zero = allocator.allocate(0); CHECK(zero != nullptr); CHECK(resource.snapshot().backing_requested_bytes == 1);
    allocator.deallocate(zero, 0);
    owner.begin_close(); CHECK_THROWS_AS(allocator.allocate(1), std::bad_alloc);
    Allocator<int> empty{ResourceHandle{}}; CHECK_THROWS_AS(empty.allocate(1), std::bad_alloc);
}

TEST_CASE("ownership Buffer resize commits only after successful allocation and retains alignment")
{
    Observed observed; auto owner = observed.make(); auto resource = heap(owner, 160);
    auto value = try_allocate(resource, 64, 256); REQUIRE(value);
    std::memset(value->data(), 0x4b, value->size()); auto* original = value->data();
    CHECK(value->try_resize(128).error() == AllocationError::limit_exceeded);
    CHECK(value->data() == original); CHECK(value->size() == 64); CHECK(observed.allocations == 1);
    observed.fail = true;
    CHECK(value->try_resize(80).error() == AllocationError::out_of_memory);
    CHECK(value->data() == original); CHECK(value->bytes()[63] == std::byte{0x4b});
    CHECK(resource.snapshot().reserved_bytes == 64); CHECK(observed.allocations == 1); CHECK(observed.frees == 0);
    observed.fail = false; REQUIRE(value->try_resize(80));
    CHECK(value->size() == 80); CHECK(value->bytes()[63] == std::byte{0x4b});
    CHECK(reinterpret_cast<std::uintptr_t>(value->data()) % 256 == 0);
    REQUIRE(value->try_resize(8)); CHECK(value->bytes()[7] == std::byte{0x4b});
    REQUIRE(value->try_resize(0)); CHECK(bool(*value)); CHECK(value->bytes().empty());
    CHECK(resource.snapshot().backing_requested_bytes == 1);
    owner.begin_close(); CHECK(value->try_resize(1).error() == AllocationError::closing);
    CHECK(value->try_resize(0)); value->reset(); CHECK(owner.try_close().closed());
    CHECK(observed.allocations == observed.frees);
}

TEST_CASE("ownership Buffer move assignment releases the displaced allocation with its original owner")
{
    Observed left_observed, right_observed;
    Buffer left, right;
    {
        auto a = left_observed.make(); auto b = right_observed.make();
        auto x = try_allocate(heap(a), 13, 32); auto y = try_allocate(heap(b), 21, 64);
        REQUIRE(x); REQUIRE(y); left = std::move(*x); right = std::move(*y);
        left.bytes()[0] = std::byte{9};
    }
    right = std::move(left); CHECK_FALSE(left); CHECK(right.size() == 13); CHECK(right.bytes()[0] == std::byte{9});
    CHECK(right_observed.destroyed == 1); CHECK(left_observed.destroyed == 0);
    right = std::move(right); CHECK(right.size() == 13);
    std::jthread consumer{[value = std::move(right)]() mutable { value.reset(); }}; consumer.join();
    CHECK(left_observed.destroyed == 1);
    CHECK(left.try_resize(4).error() == AllocationError::invalid_handle);
}

TEST_CASE("ownership factories clean object and member allocations when construction throws")
{
    auto owner = system(); auto resource = heap(owner); ThreadContext context{owner}; ExecutionScope scope{context, resource};
    CHECK_THROWS_AS(make_unique<Throws>(), std::runtime_error);
    CHECK(resource.snapshot().live_allocations == 0);
    CHECK_THROWS_AS(make_shared<Throws>(), std::runtime_error);
    CHECK(resource.snapshot().live_allocations == 0);
    auto bounded = heap(owner, 1);
    CHECK_THROWS_AS(make_unique_in<Aligned>(bounded), std::bad_alloc);
    CHECK_THROWS_AS(make_shared_in<Aligned>(bounded), std::bad_alloc);
    CHECK(bounded.snapshot().live_allocations == 0);
}

TEST_CASE("ownership unique and shared support over-aligned objects and foreign destruction")
{
    Observed observed; UniquePtr<Aligned> unique; std::shared_ptr<Aligned> shared;
    {
        auto owner = observed.make(); auto resource = heap(owner);
        std::jthread producer{[&, local = std::move(owner), resource] {
            unique = make_unique_in<Aligned>(resource); shared = make_shared_in<Aligned>(resource);
        }}; producer.join();
    }
    REQUIRE(unique); REQUIRE(shared); CHECK(unique->value == 17); CHECK(shared->value == 17);
    CHECK(reinterpret_cast<std::uintptr_t>(unique.get()) % 256 == 0);
    CHECK(reinterpret_cast<std::uintptr_t>(shared.get()) % 256 == 0);
    CHECK(observed.destroyed == 0); unique.reset(); CHECK(observed.destroyed == 0);
    shared.reset(); unique = UniquePtr<Aligned>{}; // reset/null assignment retains the deleter's owner.
    CHECK(observed.destroyed == 1); CHECK(observed.allocations == observed.frees);
    static_assert(!std::is_move_constructible_v<ThreadContext>);
    static_assert(!std::is_move_constructible_v<ExecutionScope>);
    static_assert(!std::is_move_constructible_v<DomainScope>);
    struct Base { virtual ~Base() = default; }; struct Derived : Base {};
    static_assert(!std::is_constructible_v<UniquePtr<Base>, UniquePtr<Derived>>);
}

TEST_CASE("ownership factories support const results without const qualified storage allocators")
{
    auto owner = system(); auto resource = heap(owner);
    {
        auto unique = make_unique_in<const Aligned>(resource);
        auto shared = make_shared_in<const Aware>(resource, 42);
        CHECK(unique->value == 17); CHECK(shared->values.front() == 42);
        CHECK(shared->values.get_allocator().resource() == resource);
    }
    CHECK(resource.snapshot().live_allocations == 0);
}

TEST_CASE("ownership weak pointer retains the heap after object and system destruction")
{
    Observed observed; std::weak_ptr<Aligned> weak;
    {
        auto owner = observed.make(); auto resource = heap(owner);
        auto value = make_shared_in<Aligned>(resource); weak = value;
        value.reset(); CHECK(weak.expired()); CHECK(resource.snapshot().live_allocations > 0);
        CHECK_FALSE(owner.try_close().closed());
    }
    CHECK(observed.destroyed == 0);
    std::jthread consumer{[value = std::move(weak)]() mutable { value.reset(); }}; consumer.join();
    CHECK(observed.destroyed == 1); CHECK(observed.allocations == observed.frees);
}

TEST_CASE("ownership missing context is explicit and explicit APIs do not create a route")
{
    CHECK(try_current_resource().error() == ContextErrorCode::missing_context);
    context_error([] { dk::Vector<int> value; }, ContextErrorCode::missing_context);
    context_error([] { auto value = make_shared<int>(3); }, ContextErrorCode::missing_context);
    context_error([] { auto value = make_unique<int>(3); }, ContextErrorCode::missing_context);
    auto owner = system(); auto resource = heap(owner);
    auto value = make_shared_in<int>(resource, 6); CHECK(*value == 6);
    CHECK(try_current_resource().error() == ContextErrorCode::missing_context);
    context_error([&] { DomainScope scope{resource}; }, ContextErrorCode::missing_context);
    context_error([&] { auto plain = make_unique_in<Plain>(resource); }, ContextErrorCode::missing_context);
}

TEST_CASE("ownership nested domains and systems restore routes after exceptions")
{
    auto a = system(); auto b = system(); auto first = heap(a); auto second = heap(a); auto foreign = heap(b);
    ThreadContext ca{a}, cb{b};
    {
        ExecutionScope outer{ca, first}; CHECK(current_resource() == first);
        try {
            DomainScope local{second}; CHECK(current_resource() == second);
            ExecutionScope inner{cb, foreign}; CHECK(current_resource() == foreign);
            throw std::runtime_error("leave both scopes");
        } catch (const std::runtime_error&) {}
        CHECK(current_resource() == first);
        context_error([&] { DomainScope wrong{foreign}; }, ContextErrorCode::wrong_system);
        context_error([&] { ExecutionScope wrong{ca, foreign}; }, ContextErrorCode::wrong_system);
        context_error([&] { DomainScope empty{ResourceHandle{}}; }, ContextErrorCode::invalid_resource);
        CHECK(current_resource() == first);
    }
    CHECK(try_current_resource().error() == ContextErrorCode::missing_context);
}

TEST_CASE("ownership implicit containers keep captured resources after scope changes and thread transfer")
{
    auto owner = system(); auto a = heap(owner); auto b = heap(owner); ThreadContext context{owner};
    std::optional<dk::Vector<int>> result;
    {
        ExecutionScope scope{context, a}; result.emplace(32, 4);
        DomainScope domain{b}; result->resize(256);
        CHECK(result->get_allocator().resource() == a);
        dk::String text(96, 'a'); CHECK(text.get_allocator().resource() == b);
    }
    std::atomic<bool> good{false};
    std::jthread consumer{[value = std::move(*result), a, &good]() mutable {
        value.resize(512); good = value.get_allocator().resource() == a && value[0] == 4 && !try_current_resource();
    }}; consumer.join(); CHECK(good);
}

TEST_CASE("ownership explicit factories use allocator aware construction without overriding ordinary members")
{
    auto owner = system(); auto a = heap(owner); auto b = heap(owner); ThreadContext context{owner};
    ExecutionScope scope{context, b};
    auto implicit = make_shared<Plain>(); CHECK(implicit->values.get_allocator().resource() == b);
    auto ordinary = make_unique_in<Plain>(a); CHECK(ordinary->values.get_allocator().resource() == b);
    CHECK(ordinary.get_deleter().resource == a);
    auto aware_unique = make_unique_in<Aware>(a, 8); auto aware_shared = make_shared_in<Aware>(a, 9);
    CHECK(aware_unique->values.get_allocator().resource() == a); CHECK(aware_unique->values[0] == 8);
    CHECK(aware_shared->values.get_allocator().resource() == a); CHECK(aware_shared->values[0] == 9);
    CHECK(current_resource() == b);
    using Pair = std::pair<dk::String, dk::Vector<int>>;
    auto pair = make_shared_in<Pair>(a, std::piecewise_construct, std::forward_as_tuple(80, 'x'), std::forward_as_tuple(32, 2));
    CHECK(pair->first.get_allocator().resource() == a); CHECK(pair->second.get_allocator().resource() == a);
}

TEST_CASE("ownership scoped allocator routes nested containers explicitly across domains")
{
    auto owner = system(); auto a = heap(owner); auto b = heap(owner); auto c = heap(owner);
    ThreadContext context{owner}; ExecutionScope scope{context, c};
    using Inner = dk::Vector<int>;
    using NestedAllocator = std::scoped_allocator_adaptor<Allocator<Inner>, Allocator<int>>;
    std::vector<Inner, NestedAllocator> values{NestedAllocator{Allocator<Inner>{a}, Allocator<int>{b}}};
    values.emplace_back(32, 7);
    CHECK(values.get_allocator().outer_allocator().resource() == a);
    CHECK(values.front().get_allocator().resource() == b); CHECK(values.front()[0] == 7);
    CHECK(current_resource() == c);
}

TEST_CASE("ownership contexts isolate threads reject wrong thread entry and keep closing busy")
{
    auto owner = system(); auto a = heap(owner); auto b = heap(owner);
    {
        ThreadContext main_context{owner}; ExecutionScope main_scope{main_context, a};
        std::barrier phase{2}; std::atomic<bool> correct{false};
        std::jthread worker{[&] {
            bool wrong_thread = false;
            try { ExecutionScope invalid{main_context, b}; }
            catch (const ContextError& e) { wrong_thread = e.code() == ContextErrorCode::wrong_thread; }
            const bool empty = !try_current_resource();
            {
                ThreadContext local{owner}; ExecutionScope worker_scope{local, b};
                correct = wrong_thread && empty && current_resource() == b;
                phase.arrive_and_wait(); phase.arrive_and_wait();
            }
            correct = correct && !try_current_resource();
        }};
        phase.arrive_and_wait();
        CHECK(current_resource() == a);
        const auto close = owner.try_close();
        const auto route = try_current_resource();
        phase.arrive_and_wait(); worker.join();
        CHECK(correct); CHECK_FALSE(close.closed()); CHECK(close.active_contexts == 2);
        CHECK(route.error() == ContextErrorCode::closing);
        context_error([&] { ThreadContext late{owner}; }, ContextErrorCode::closing);
        context_error([&] { DomainScope late{a}; }, ContextErrorCode::closing);
    }
    CHECK(owner.try_close().closed());
}

TEST_CASE("ownership context and scope remain safe after the system wrapper is destroyed")
{
    auto owner = std::make_unique<MemorySystem>(system()); auto resource = heap(*owner);
    ThreadContext context{*owner};
    {
        ExecutionScope scope{context, resource}; auto value = make_unique<int>(12);
        owner.reset(); CHECK(context.state() == ResourceState::closing); CHECK(*value == 12);
        CHECK(try_current_resource().error() == ContextErrorCode::closing);
    }
    CHECK(resource.snapshot().live_allocations == 0);
}

TEST_CASE("ownership wrappers emit exactly one heap event per backing allocation")
{
    Observed observed; auto owner = observed.make(); auto resource = heap(owner); ThreadContext context{owner};
    {
        ExecutionScope scope{context, resource};
        auto bytes = try_allocate(resource, 64); REQUIRE(bytes);
        auto unique = make_unique<int>(3); auto shared = make_shared<int>(4);
        dk::Vector<int> vector(32, 5);
        std::pmr::vector<int> pmr{resource.pmr_resource()}; pmr.resize(32);
        CHECK(static_cast<std::uint64_t>(observed.allocations.load()) == resource.snapshot().allocation_count);
        CHECK(observed.frees == 0);
    }
    CHECK(resource.snapshot().live_allocations == 0); CHECK(observed.allocations == observed.frees);
}

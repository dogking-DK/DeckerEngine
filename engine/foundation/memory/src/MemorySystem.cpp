#include <dk/memory/MemorySystem.hpp>
#include <dk/memory/Context.hpp>
#include <dk/profiling/Memory.hpp>
#include "MemoryInternal.hpp"

#include <mimalloc.h>
#include <atomic>
#include <bit>
#include <cassert>
#include <exception>
#include <limits>
#include <mutex>
#include <new>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

static_assert(MI_MALLOC_VERSION >= 30000 && MI_MALLOC_VERSION < 40000, "Memory requires mimalloc v3 heaps");

namespace dk::memory {
namespace {
constexpr auto closing_bit = std::uint64_t{1} << 63;
constexpr auto maintenance_bit = std::uint64_t{1} << 62;
constexpr auto closed_bit = std::uint64_t{1} << 61;
constexpr auto count_mask = closed_bit - 1;
std::atomic<SystemId> next_system_id{1}; // Identity only; never an allocation router.

std::size_t normalized(std::size_t bytes) noexcept { return bytes == 0 ? 1 : bytes; }
void saturating_add(std::size_t& sum, std::size_t value) noexcept
{
    const auto maximum = (std::numeric_limits<std::size_t>::max)();
    sum = value > maximum - sum ? maximum : sum + value;
}
profiling::HeapCategory trace_category(DomainCategory value) noexcept
{
    switch (value) {
    case DomainCategory::general: return profiling::HeapCategory::general;
    case DomainCategory::assets: return profiling::HeapCategory::assets;
    case DomainCategory::scene: return profiling::HeapCategory::scene;
    case DomainCategory::render: return profiling::HeapCategory::render;
    case DomainCategory::jobs: return profiling::HeapCategory::jobs;
    default: return profiling::HeapCategory::other;
    }
}
detail::EventSink default_sink() noexcept
{
    return {nullptr,
        [](void*, const void* p, std::size_t n, DomainCategory c) noexcept {
            profiling::record_allocation(p, n, trace_category(c));
        },
        [](void*, const void* p, DomainCategory c) noexcept {
            profiling::record_free(p, trace_category(c));
        }};
}
} // namespace

namespace detail {
struct SystemState {
    SystemId id;
    std::mutex registry_mutex; // Shared with token-based context registration, even after wrapper destruction.
    std::atomic<ResourceState> state{ResourceState::open};
    std::atomic<std::size_t> contexts{0};
    Backend backend;
    EventSink sink;
    SystemState(SystemId identity, Backend b, EventSink s) noexcept : id(identity), backend(b), sink(s) {}
};

struct ResourceControl : std::pmr::memory_resource {
    std::shared_ptr<SystemState> system;
    DomainId id;
    std::string name;
    DomainCategory category;
    std::size_t budget;
    void* heap = nullptr; // Access only with a gate ticket or exclusive maintenance.
    std::atomic<std::uint64_t> gate{0};
    std::atomic<std::size_t> reserved{0}, backing{0}, peak{0}, live{0};
    std::atomic<std::uint64_t> allocations{0}, failures{0};

    ResourceControl(std::shared_ptr<SystemState> s, DomainId domain, HeapOptions options)
        : system(std::move(s)), id(domain), name(options.name), category(options.category), budget(options.budget_bytes) {}
    ~ResourceControl()
    {
        // A raw pointer without a retained owner violates the public contract.
        if (!close().closed()) { std::terminate(); }
    }

    void* do_allocate(std::size_t bytes, std::size_t alignment) override
    {
        auto result = allocate(bytes, alignment);
        if (!result) { throw std::bad_alloc{}; }
        return *result;
    }
    void do_deallocate(void* pointer, std::size_t bytes, std::size_t alignment) override
    { deallocate(pointer, bytes, alignment); }
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override { return this == &other; }

    bool enter(bool allocating) noexcept
    {
        auto value = gate.load(std::memory_order_acquire);
        for (;;) {
            if ((value & closed_bit) || (allocating && (value & closing_bit))) { return false; }
            if (value & maintenance_bit) {
                std::this_thread::yield();
                value = gate.load(std::memory_order_acquire);
                continue;
            }
            if ((value & count_mask) == count_mask) { std::terminate(); }
            if (gate.compare_exchange_weak(value, value + 1, std::memory_order_acq_rel)) {
                if (allocating && system->state.load() != ResourceState::open) {
                    leave();
                    return false;
                }
                return true;
            }
        }
    }
    void leave() noexcept { gate.fetch_sub(1, std::memory_order_release); }
    void begin_close() noexcept { gate.fetch_or(closing_bit, std::memory_order_acq_rel); }
    ResourceSnapshot snapshot() const noexcept
    {
        const auto value = gate.load(std::memory_order_acquire);
        auto state = (value & closed_bit) ? ResourceState::closed :
            ((value & closing_bit) || system->state.load() != ResourceState::open) ? ResourceState::closing : ResourceState::open;
        return {system->id, id, state, budget, reserved.load(), backing.load(), peak.load(), live.load(),
                static_cast<std::size_t>(value & count_mask), allocations.load(), failures.load()};
    }
    CloseResult close() noexcept
    {
        begin_close();
        auto expected = closing_bit;
        if (gate.compare_exchange_strong(expected, closing_bit | maintenance_bit, std::memory_order_acq_rel)) {
            // Check AFTER acquiring exclusion: an admitted allocation may have
            // committed live storage just before it released its gate ticket.
            if (live.load(std::memory_order_acquire) == 0) {
                if (heap) { system->backend.destroy(system->backend.context, heap); heap = nullptr; }
                gate.store(closing_bit | closed_bit, std::memory_order_release);
            } else {
                gate.store(closing_bit, std::memory_order_release);
            }
        }
        auto value = snapshot();
        // Maintenance is a busy operation, even though it doesn't hold a ticket.
        if (gate.load(std::memory_order_acquire) & maintenance_bit) { ++value.active_operations; }
        return {value.state, value.live_allocations, value.active_operations, value.backing_requested_bytes};
    }
    std::expected<void*, AllocationError> allocate(std::size_t bytes, std::size_t alignment) noexcept
    {
        auto fail = [this](AllocationError error) -> std::expected<void*, AllocationError> {
            failures.fetch_add(1, std::memory_order_relaxed);
            return std::unexpected(error);
        };
        if (!std::has_single_bit(alignment)) { return fail(AllocationError::invalid_alignment); }
        const auto size = normalized(bytes);
        constexpr auto maximum = static_cast<std::size_t>((std::numeric_limits<std::ptrdiff_t>::max)());
        if (alignment > maximum || size > maximum - (alignment - 1)) { return fail(AllocationError::size_overflow); }
        if (!enter(true)) { return fail(AllocationError::closing); }
        struct Ticket { ResourceControl& owner; ~Ticket() { owner.leave(); } } ticket{*this};
        auto current = reserved.load(std::memory_order_relaxed);
        for (;;) {
            if (size > (std::numeric_limits<std::size_t>::max)() - current) { return fail(AllocationError::size_overflow); }
            if (budget && (current > budget || size > budget - current)) { return fail(AllocationError::limit_exceeded); }
            if (reserved.compare_exchange_weak(current, current + size, std::memory_order_acq_rel)) { break; }
        }
        void* p = system->backend.allocate(system->backend.context, heap, size, alignment);
        if (!p) {
            reserved.fetch_sub(size, std::memory_order_release);
            return fail(AllocationError::out_of_memory);
        }
        // Emit before publication; the matching free must precede address reuse.
        if (system->sink.allocate) { system->sink.allocate(system->sink.context, p, size, category); }
        const auto now = backing.fetch_add(size, std::memory_order_relaxed) + size;
        auto previous_peak = peak.load(std::memory_order_relaxed);
        while (previous_peak < now && !peak.compare_exchange_weak(previous_peak, now, std::memory_order_relaxed)) {}
        live.fetch_add(1, std::memory_order_relaxed);
        allocations.fetch_add(1, std::memory_order_relaxed);
        return p;
    }
    void deallocate(void* p, std::size_t bytes, std::size_t alignment) noexcept
    {
        if (!enter(false)) { std::terminate(); }
        if (system->sink.free) { system->sink.free(system->sink.context, p, category); }
        system->backend.free(system->backend.context, heap, p, normalized(bytes), alignment);
        backing.fetch_sub(normalized(bytes), std::memory_order_relaxed);
        reserved.fetch_sub(normalized(bytes), std::memory_order_release);
        live.fetch_sub(1, std::memory_order_relaxed);
        leave();
    }
};

Backend mimalloc_backend() noexcept
{
    return {nullptr,
        [](void*) noexcept -> void* { return mi_heap_new(); },
        [](void*, void* h) noexcept { mi_heap_delete(static_cast<mi_heap_t*>(h)); },
        [](void*, void* h, std::size_t n, std::size_t a) noexcept -> void* {
            return mi_heap_malloc_aligned(static_cast<mi_heap_t*>(h), n, a);
        },
        [](void*, void* h, void* p, std::size_t, std::size_t) noexcept {
            assert(mi_heap_contains(static_cast<mi_heap_t*>(h), p));
            (void)h;
            mi_free(p);
        }};
}
} // namespace detail

struct MemorySystem::Impl {
    std::shared_ptr<detail::SystemState> shared;
    std::vector<std::shared_ptr<detail::ResourceControl>> resources;
    DomainId next_domain = 1;
    explicit Impl(std::shared_ptr<detail::SystemState> state) : shared(std::move(state)) {}
    ~Impl() { const auto result = close(); (void)result; }
    void begin_close_locked() noexcept
    {
        auto expected = ResourceState::open;
        shared->state.compare_exchange_strong(expected, ResourceState::closing);
        for (const auto& resource : resources) { resource->begin_close(); }
    }
    CloseResult close() noexcept
    {
        std::lock_guard lock{shared->registry_mutex};
        begin_close_locked();
        CloseResult result;
        result.active_contexts = shared->contexts.load(std::memory_order_acquire);
        if (result.active_contexts) { result.state = ResourceState::closing; }
        for (const auto& resource : resources) {
            const auto sample = resource->snapshot();
            const auto item = result.active_contexts
                ? CloseResult{sample.state, sample.live_allocations, sample.active_operations, sample.backing_requested_bytes}
                : resource->close();
            if (!item.closed()) { result.state = ResourceState::closing; }
            saturating_add(result.live_allocations, item.live_allocations);
            saturating_add(result.active_operations, item.active_operations);
            saturating_add(result.backing_requested_bytes, item.backing_requested_bytes);
        }
        if (result.closed()) { shared->state.store(ResourceState::closed); }
        return result;
    }
};

std::expected<MemorySystem, AllocationError> detail::MemoryAccess::create(Backend backend, EventSink sink) noexcept
{
    if (!backend.create || !backend.destroy || !backend.allocate || !backend.free) {
        return std::unexpected(AllocationError::invalid_argument);
    }
    auto id = next_system_id.load(std::memory_order_relaxed);
    do {
        if (id == (std::numeric_limits<SystemId>::max)()) { return std::unexpected(AllocationError::limit_exceeded); }
    } while (!next_system_id.compare_exchange_weak(id, id + 1, std::memory_order_relaxed));
    try {
        auto shared = std::make_shared<SystemState>(id, backend, sink);
        return MemorySystem{std::make_unique<MemorySystem::Impl>(std::move(shared))};
    } catch (const std::bad_alloc&) { return std::unexpected(AllocationError::out_of_memory); }
}

MemorySystem::MemorySystem(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
MemorySystem::~MemorySystem() = default;
MemorySystem::MemorySystem(MemorySystem&&) noexcept = default;
MemorySystem& MemorySystem::operator=(MemorySystem&&) noexcept = default;
std::expected<MemorySystem, AllocationError> MemorySystem::create() noexcept
{
    return detail::MemoryAccess::create(detail::mimalloc_backend(), default_sink());
}
SystemId MemorySystem::id() const noexcept { return impl_ ? impl_->shared->id : 0; }
ResourceState MemorySystem::state() const noexcept { return impl_ ? impl_->shared->state.load() : ResourceState::closed; }
std::expected<ResourceHandle, AllocationError> MemorySystem::create_heap(HeapOptions options) noexcept
{
    if (!impl_) { return std::unexpected(AllocationError::invalid_handle); }
    if (static_cast<unsigned>(options.category) > static_cast<unsigned>(DomainCategory::other)) {
        return std::unexpected(AllocationError::invalid_argument);
    }
    std::lock_guard lock{impl_->shared->registry_mutex};
    if (state() != ResourceState::open) { return std::unexpected(AllocationError::closing); }
    if (impl_->next_domain == (std::numeric_limits<DomainId>::max)()) { return std::unexpected(AllocationError::limit_exceeded); }
    try {
        auto resource = std::make_shared<detail::ResourceControl>(impl_->shared, impl_->next_domain, options);
        resource->heap = impl_->shared->backend.create(impl_->shared->backend.context);
        if (!resource->heap) { return std::unexpected(AllocationError::out_of_memory); }
        impl_->resources.push_back(resource); // Publication; failure destroys the candidate heap.
        ++impl_->next_domain;
        return ResourceHandle{std::move(resource)};
    } catch (const std::bad_alloc&) { return std::unexpected(AllocationError::out_of_memory); }
      catch (const std::length_error&) { return std::unexpected(AllocationError::size_overflow); }
}
void MemorySystem::begin_close() noexcept
{
    if (!impl_) { return; }
    std::lock_guard lock{impl_->shared->registry_mutex};
    impl_->begin_close_locked();
}
CloseResult MemorySystem::try_close() noexcept { return impl_ ? impl_->close() : CloseResult{}; }

ThreadContext::ThreadContext(MemorySystem& system)
    : ThreadContext(system.impl_ ? system.impl_->shared : nullptr) {}
ThreadContext::ThreadContext(std::shared_ptr<detail::SystemState> system) : thread_(std::this_thread::get_id())
{
    if (!system) { throw ContextError{ContextErrorCode::invalid_resource}; }
    std::lock_guard lock{system->registry_mutex};
    if (system->state.load() != ResourceState::open) { throw ContextError{ContextErrorCode::closing}; }
    system_ = std::move(system);
    system_->contexts.fetch_add(1, std::memory_order_release);
}
ThreadContext::~ThreadContext()
{
    if (thread_ != std::this_thread::get_id() || scopes_ != 0) { std::terminate(); }
    local_pool_.reset();
    scratch_.reset(); // Drop all local backing while the context lease still prevents system close.
    system_->contexts.fetch_sub(1, std::memory_order_release);
}
SystemId ThreadContext::system_id() const noexcept { return system_->id; }
ResourceState ThreadContext::state() const noexcept { return system_->state.load(); }

RoutingToken RoutingToken::from_resource(ResourceHandle resource)
{
    if (!resource) { throw ContextError{ContextErrorCode::invalid_resource}; }
    RoutingToken token;
    token.system_ = resource.control_->system;
    token.resource_ = std::move(resource);
    const auto valid = token.try_validate();
    if (!valid) { throw ContextError{valid.error()}; }
    return token;
}
std::expected<void, ContextErrorCode> RoutingToken::try_validate() const noexcept
{
    if (!*this) { return std::unexpected(ContextErrorCode::invalid_token); }
    if (system_->state.load() != ResourceState::open || resource_.state() != ResourceState::open) {
        return std::unexpected(ContextErrorCode::closing);
    }
    return {};
}

ResourceHandle::ResourceHandle(std::shared_ptr<detail::ResourceControl> control) noexcept : control_(std::move(control)) {}
SystemId ResourceHandle::system_id() const noexcept { return control_ ? control_->system->id : 0; }
DomainId ResourceHandle::domain_id() const noexcept { return control_ ? control_->id : 0; }
std::string_view ResourceHandle::name() const noexcept { return control_ ? std::string_view{control_->name} : std::string_view{}; }
DomainCategory ResourceHandle::category() const noexcept { return control_ ? control_->category : DomainCategory::other; }
ResourceState ResourceHandle::state() const noexcept
{
    if (!control_) { return ResourceState::closed; }
    const auto value = control_->gate.load(std::memory_order_acquire);
    if (value & closed_bit) { return ResourceState::closed; }
    return (value & closing_bit) || control_->system->state.load() != ResourceState::open
        ? ResourceState::closing : ResourceState::open;
}
ResourceSnapshot ResourceHandle::snapshot() const noexcept { return control_ ? control_->snapshot() : ResourceSnapshot{}; }
std::pmr::memory_resource* ResourceHandle::pmr_resource() const noexcept
{ return control_ ? static_cast<std::pmr::memory_resource*>(control_.get()) : std::pmr::null_memory_resource(); }
std::expected<void*, AllocationError> ResourceHandle::try_allocate(std::size_t bytes, std::size_t alignment) const noexcept
{
    if (!control_) { return std::unexpected(AllocationError::invalid_handle); }
    return control_->allocate(bytes, alignment);
}
void ResourceHandle::deallocate(void* p, std::size_t bytes, std::size_t alignment) const noexcept
{
    if (!p) { return; }
    if (!control_) { std::terminate(); }
    control_->deallocate(p, bytes, alignment);
}
void ResourceHandle::begin_close() const noexcept { if (control_) { control_->begin_close(); } }
CloseResult ResourceHandle::try_close() const noexcept { return control_ ? control_->close() : CloseResult{}; }
std::expected<std::size_t, AllocationError> checked_byte_size(std::size_t count, std::size_t size) noexcept
{
    if (size && count > (std::numeric_limits<std::size_t>::max)() / size) { return std::unexpected(AllocationError::size_overflow); }
    return count * size;
}
} // namespace dk::memory

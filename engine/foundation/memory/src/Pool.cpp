#include <dk/memory/Pool.hpp>
#include <dk/profiling/Memory.hpp>
#include <atomic>
#include <array>
#include <bit>
#include <limits>
#include <mutex>
#include <thread>
#include <utility>
#include <variant>

namespace dk::memory {
namespace {
constexpr auto closing_bit = std::uint64_t{1} << 63;
constexpr auto maintenance_bit = std::uint64_t{1} << 62;
constexpr auto closed_bit = std::uint64_t{1} << 61;
constexpr auto count_mask = closed_bit - 1;
constexpr auto size_limit = static_cast<std::size_t>((std::numeric_limits<std::ptrdiff_t>::max)());
void update_peak(std::atomic<std::size_t>& peak, std::size_t value) noexcept
{
    auto old = peak.load();
    while (old < value && !peak.compare_exchange_weak(old, value)) {}
}
class PoolUpstream final : public std::pmr::memory_resource {
public:
    explicit PoolUpstream(ResourceHandle resource) : owner(std::move(resource)) {}
    ResourceHandle owner;
    std::atomic<std::size_t> backing{0}, peak{0};
    std::atomic<std::uint64_t> allocations{0}, total_bytes{0};
    void begin_construction() noexcept
    {
#if defined(_MSC_VER) && _ITERATOR_DEBUG_LEVEL > 0
        constructing_ = true; construction_used_ = 0;
#endif
    }
    void end_construction() noexcept
    {
#if defined(_MSC_VER) && _ITERATOR_DEBUG_LEVEL > 0
        constructing_ = false;
#endif
    }
private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override
    {
#if defined(_MSC_VER) && _ITERATOR_DEBUG_LEVEL > 0
        // MSVC's noexcept pool constructor allocates an iterator proxy. Keep that
        // constructor-only metadata inside the bootstrap control, away from fallible heap budgets.
        if (constructing_) {
            void* pointer = construction_storage_.data() + construction_used_;
            auto space = construction_storage_.size() - construction_used_;
            const auto normalized = bytes == 0 ? 1 : bytes;
            if (!std::align(alignment, normalized, pointer, space)) { std::terminate(); } // Unsupported STL layout.
            construction_used_ = static_cast<std::size_t>(static_cast<std::byte*>(pointer) - construction_storage_.data()) + normalized;
            return pointer;
        }
#endif
        DK_PROFILE_ZONE("Memory.Pool.Grow");
        auto result = owner.try_allocate(bytes, alignment);
        if (!result) { throw PoolError{result.error()}; }
        const auto normalized = bytes == 0 ? 1 : bytes;
        update_peak(peak, backing.fetch_add(normalized) + normalized);
        ++allocations; total_bytes.fetch_add(normalized);
        return *result;
    }
    void do_deallocate(void* pointer, std::size_t bytes, std::size_t alignment) override
    {
#if defined(_MSC_VER) && _ITERATOR_DEBUG_LEVEL > 0
        const auto address = reinterpret_cast<std::uintptr_t>(pointer);
        const auto start = reinterpret_cast<std::uintptr_t>(construction_storage_.data());
        if (address >= start && address - start < construction_storage_.size()) { return; }
#endif
        owner.deallocate(pointer, bytes, alignment);
        backing.fetch_sub(bytes == 0 ? 1 : bytes);
    }
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override { return this == &other; }
#if defined(_MSC_VER) && _ITERATOR_DEBUG_LEVEL > 0
    alignas(std::max_align_t) std::array<std::byte, 256> construction_storage_{};
    std::size_t construction_used_ = 0;
    bool constructing_ = false;
#endif
};
}
namespace detail {
class PoolControl final : public std::pmr::memory_resource {
public:
    PoolControl(ResourceHandle resource, PoolOptions options, bool shared)
        : shared_(shared), thread_(std::this_thread::get_id()), upstream_(std::move(resource)), requested_options_(options)
    {
        if (!upstream_.owner) { throw PoolError{AllocationError::invalid_handle}; }
        if (upstream_.owner.state() != ResourceState::open) { throw PoolError{AllocationError::closing}; }
        ensure_pool();
        actual_options_ = shared_ ? std::get<2>(pool_).options() : std::get<1>(pool_).options();
        sample_unlocked();
    }
    ~PoolControl() override
    {
        if (!allowed_thread() || (gate_.load() & (count_mask | maintenance_bit)) || live_.load() != 0) { std::terminate(); }
        (void)close();
    }
    bool allowed_thread() const noexcept { return shared_ || thread_ == std::this_thread::get_id(); }
    ResourceHandle upstream() const noexcept { return upstream_.owner; }
    PoolOptions options() const noexcept
    { return actual_options_; }
    ResourceState state() const noexcept
    {
        auto gate = gate_.load();
        if (gate & closed_bit) { return ResourceState::closed; }
        return (gate & closing_bit) || upstream_.owner.state() != ResourceState::open ? ResourceState::closing : ResourceState::open;
    }
    PoolSnapshot snapshot() const noexcept
    {
        const auto live = live_.load(); const auto gate = gate_.load(); const auto backing = upstream_.backing.load();
        return {state(), logical_.load(), live, static_cast<std::size_t>(gate & count_mask), backing,
            live == 0 && (gate & count_mask) == 0 ? backing : 0, peak_.load(), upstream_.peak.load(),
            allocations_.load(), failures_.load(), upstream_.allocations.load(), upstream_.total_bytes.load()};
    }
    std::expected<void*, AllocationError> allocate_block(std::size_t bytes, std::size_t alignment) noexcept
    {
        if (!allowed_thread()) { return std::unexpected(AllocationError::wrong_thread); }
        const auto fail = [this](AllocationError error) -> std::expected<void*, AllocationError> {
            ++failures_; return std::unexpected(error);
        };
        if (!std::has_single_bit(alignment)) { return fail(AllocationError::invalid_alignment); }
        bytes = bytes == 0 ? 1 : bytes;
        if (alignment > size_limit || bytes > size_limit - (alignment - 1)) { return fail(AllocationError::size_overflow); }
        if (!enter(true)) { return fail(AllocationError::closing); }
        if (upstream_.owner.state() != ResourceState::open) { leave(); return fail(AllocationError::closing); }
        auto reserved = reserved_.load();
        do {
            if (bytes > size_limit - reserved) { leave(); return fail(AllocationError::size_overflow); }
        } while (!reserved_.compare_exchange_weak(reserved, reserved + bytes));
        void* pointer = nullptr;
        try {
            ensure_pool();
            pointer = shared_ ? std::get<2>(pool_).allocate(bytes, alignment) : std::get<1>(pool_).allocate(bytes, alignment);
        } catch (const PoolError& error) {
            reserved_.fetch_sub(bytes); leave(); return fail(error.code());
        } catch (const std::bad_alloc&) {
            reserved_.fetch_sub(bytes); leave(); return fail(AllocationError::out_of_memory);
        }
        update_peak(peak_, logical_.fetch_add(bytes) + bytes);
        ++live_; ++allocations_;
        leave(); // Publish counters before maintenance can observe zero active operations.
        return pointer;
    }
    void free_block(void* pointer, std::size_t bytes, std::size_t alignment) noexcept
    {
        if (!pointer) { return; }
        if (!allowed_thread() || !enter(false)) { std::terminate(); }
        bytes = bytes == 0 ? 1 : bytes;
        if (shared_) { std::get<2>(pool_).deallocate(pointer, bytes, alignment); }
        else { std::get<1>(pool_).deallocate(pointer, bytes, alignment); }
        logical_.fetch_sub(bytes); reserved_.fetch_sub(bytes); --live_;
        leave();
    }
    std::expected<void, AllocationError> trim() noexcept
    {
        if (!allowed_thread()) { return std::unexpected(AllocationError::wrong_thread); }
        if (!maintain()) { return std::unexpected(AllocationError::busy); }
        if (live_.load() != 0) { finish_maintenance(); return std::unexpected(AllocationError::busy); }
        DK_PROFILE_ZONE("Memory.Pool.Trim");
        sample_unlocked();
        release_pool();
        sample_unlocked();
        finish_maintenance();
        return {};
    }
    std::expected<void, AllocationError> sample() noexcept
    {
        if (!allowed_thread()) { return std::unexpected(AllocationError::wrong_thread); }
        if (!maintain()) { return std::unexpected(AllocationError::busy); }
        sample_unlocked(); finish_maintenance(); return {};
    }
    void begin_close() noexcept
    {
        if (!allowed_thread()) { std::terminate(); }
        gate_.fetch_or(closing_bit);
    }
    CloseResult close() noexcept
    {
        begin_close();
        if (maintain()) {
            if (live_.load() == 0) {
                release_pool(); gate_.fetch_or(closed_bit); sample_unlocked();
            }
            finish_maintenance();
        }
        auto value = snapshot();
        return {value.state, value.live_allocations,
            value.active_operations + ((gate_.load() & maintenance_bit) ? 1 : 0), value.backing_bytes, 0};
    }
private:
    bool enter(bool allocating) noexcept
    {
        auto value = gate_.load();
        for (;;) {
            if ((value & closed_bit) || (allocating && (value & closing_bit))) { return false; }
            if (value & maintenance_bit) { std::this_thread::yield(); value = gate_.load(); continue; }
            if ((value & count_mask) == count_mask) { std::terminate(); }
            if (gate_.compare_exchange_weak(value, value + 1)) { return true; }
        }
    }
    void leave() noexcept { gate_.fetch_sub(1); }
    bool maintain() noexcept
    {
        auto value = gate_.load();
        if (value & (count_mask | maintenance_bit)) { return false; }
        return gate_.compare_exchange_strong(value, value | maintenance_bit);
    }
    void finish_maintenance() noexcept { gate_.fetch_and(~maintenance_bit); } // Preserve concurrent begin_close.
    void release_pool() noexcept
    {
        // release() alone can retain STL bookkeeping (e.g. MSVC Debug's container proxy).
        // Maintenance excludes all users; destroy everything, then lazily reconstruct.
        pool_.emplace<0>();
        ready_.store(false, std::memory_order_release);
    }
    void ensure_pool()
    {
        if (ready_.load(std::memory_order_acquire)) { return; }
        std::lock_guard lock{initialize_mutex_};
        if (ready_.load(std::memory_order_relaxed)) { return; }
        upstream_.begin_construction();
        try {
            if (shared_) { pool_.emplace<2>(requested_options_, &upstream_); }
            else { pool_.emplace<1>(requested_options_, &upstream_); }
        } catch (...) { upstream_.end_construction(); throw; }
        upstream_.end_construction();
        ready_.store(true, std::memory_order_release);
    }
    void sample_unlocked() noexcept
    {
        profiling::PoolUsage current{logical_.load(), upstream_.backing.load(), live_.load() == 0 ? upstream_.backing.load() : 0};
        profiling::record_pool_sample(shared_ ? profiling::PoolKind::shared : profiling::PoolKind::local, sampled_, current);
        sampled_ = current;
    }
    void* do_allocate(std::size_t bytes, std::size_t alignment) override
    { auto result = allocate_block(bytes, alignment); if (!result) { throw std::bad_alloc{}; } return *result; }
    void do_deallocate(void* pointer, std::size_t bytes, std::size_t alignment) override { free_block(pointer, bytes, alignment); }
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override { return this == &other; }
    const bool shared_;
    const std::thread::id thread_;
    PoolUpstream upstream_;
    const PoolOptions requested_options_;
    PoolOptions actual_options_;
    std::variant<std::monostate, std::pmr::unsynchronized_pool_resource, std::pmr::synchronized_pool_resource> pool_;
    std::mutex initialize_mutex_;
    std::atomic<bool> ready_{false};
    std::atomic<std::uint64_t> gate_{0}, allocations_{0}, failures_{0};
    std::atomic<std::size_t> logical_{0}, reserved_{0}, live_{0}, peak_{0};
    profiling::PoolUsage sampled_;
};
} // namespace detail

LocalPoolResource::LocalPoolResource(ResourceHandle upstream, PoolOptions options)
    : control_(std::make_unique<detail::PoolControl>(std::move(upstream), options, false)) {}
LocalPoolResource::~LocalPoolResource() = default;
bool LocalPoolResource::on_owner_thread() const noexcept { return control_->allowed_thread(); }
ResourceHandle LocalPoolResource::upstream() const noexcept { return control_->upstream(); }
PoolOptions LocalPoolResource::options() const noexcept { return control_->options(); }
ResourceState LocalPoolResource::state() const noexcept { return control_->state(); }
PoolSnapshot LocalPoolResource::snapshot() const
{ if (!on_owner_thread()) { throw PoolError{AllocationError::wrong_thread}; } return control_->snapshot(); }
std::pmr::memory_resource* LocalPoolResource::pmr_resource() noexcept { return control_.get(); }
std::expected<void*, AllocationError> LocalPoolResource::try_allocate(std::size_t n, std::size_t a) noexcept
{ return control_->allocate_block(n, a); }
void LocalPoolResource::deallocate(void* p, std::size_t n, std::size_t a) noexcept { control_->free_block(p, n, a); }
std::expected<void, AllocationError> LocalPoolResource::try_trim() noexcept { return control_->trim(); }
std::expected<void, AllocationError> LocalPoolResource::try_sample() noexcept { return control_->sample(); }
void LocalPoolResource::begin_close() noexcept { control_->begin_close(); }
std::expected<CloseResult, AllocationError> LocalPoolResource::try_close() noexcept
{ if (!on_owner_thread()) { return std::unexpected(AllocationError::wrong_thread); } return control_->close(); }

std::expected<SharedPoolResource, AllocationError> SharedPoolResource::create(ResourceHandle upstream, PoolOptions options) noexcept
{
    try {
        SharedPoolResource result;
        result.control_ = std::make_shared<detail::PoolControl>(std::move(upstream), options, true);
        return result;
    } catch (const PoolError& error) { return std::unexpected(error.code()); }
    catch (const std::bad_alloc&) { return std::unexpected(AllocationError::out_of_memory); }
}
ResourceHandle SharedPoolResource::upstream() const noexcept { return control_ ? control_->upstream() : ResourceHandle{}; }
PoolOptions SharedPoolResource::options() const noexcept { return control_ ? control_->options() : PoolOptions{}; }
ResourceState SharedPoolResource::state() const noexcept { return control_ ? control_->state() : ResourceState::closed; }
PoolSnapshot SharedPoolResource::snapshot() const noexcept { return control_ ? control_->snapshot() : PoolSnapshot{}; }
std::pmr::memory_resource* SharedPoolResource::pmr_resource() const noexcept
{ return control_ ? static_cast<std::pmr::memory_resource*>(control_.get()) : std::pmr::null_memory_resource(); }
std::expected<void*, AllocationError> SharedPoolResource::try_allocate(std::size_t n, std::size_t a) const noexcept
{ return control_ ? control_->allocate_block(n, a) : std::expected<void*, AllocationError>{std::unexpected(AllocationError::invalid_handle)}; }
void SharedPoolResource::deallocate(void* p, std::size_t n, std::size_t a) const noexcept
{ if (!p) { return; } if (!control_) { std::terminate(); } control_->free_block(p, n, a); }
std::expected<void, AllocationError> SharedPoolResource::try_trim() const noexcept
{ return control_ ? control_->trim() : std::expected<void, AllocationError>{std::unexpected(AllocationError::invalid_handle)}; }
std::expected<void, AllocationError> SharedPoolResource::try_sample() const noexcept
{ return control_ ? control_->sample() : std::expected<void, AllocationError>{std::unexpected(AllocationError::invalid_handle)}; }
void SharedPoolResource::begin_close() const noexcept { if (control_) { control_->begin_close(); } }
CloseResult SharedPoolResource::try_close() const noexcept { return control_ ? control_->close() : CloseResult{}; }
} // namespace dk::memory

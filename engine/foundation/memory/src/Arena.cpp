#include <dk/memory/Arena.hpp>
#include <dk/memory/Buffer.hpp>
#include <dk/profiling/Memory.hpp>
#include <algorithm>
#include <atomic>
#include <bit>
#include <limits>
#include <new>
#include <utility>

namespace dk::memory {
namespace {
std::atomic<std::uint64_t> next_arena_id{1}; // Cold identity allocation, never a memory router.
constexpr auto size_limit = static_cast<std::size_t>((std::numeric_limits<std::ptrdiff_t>::max)());
}
struct ScratchArena::Chunk {
    Buffer storage;
    std::size_t offset = 0;
    Chunk* next = nullptr;
    void* fit(std::size_t bytes, std::size_t alignment) noexcept
    {
        void* pointer = storage.data() + offset;
        auto space = storage.size() - offset;
        return std::align(alignment, bytes, pointer, space);
    }
};
ScratchArena::ScratchArena(ResourceHandle upstream, ScratchOptions options)
    : upstream_(std::move(upstream)), options_(options), thread_(std::this_thread::get_id())
{
    if (!upstream_) { throw ScratchError{AllocationError::invalid_handle}; }
    if (options.chunk_bytes == 0 || options.chunk_bytes > size_limit) { throw ScratchError{AllocationError::invalid_argument}; }
    if (upstream_.state() != ResourceState::open) { throw ScratchError{AllocationError::closing}; }
    identity_ = next_arena_id.fetch_add(1, std::memory_order_relaxed);
    if (identity_ == 0) { std::terminate(); }
}
ScratchArena::~ScratchArena()
{
    if (!owner_thread() || stats_.active_scopes != 0 || active_) { std::terminate(); }
    clear_cache();
    sample();
}
bool ScratchArena::owner_thread() const noexcept { return thread_ == std::this_thread::get_id(); }
ScratchSnapshot ScratchArena::snapshot() const
{
    if (!owner_thread()) { throw ScratchError{AllocationError::wrong_thread}; }
    return stats_;
}
std::expected<ScratchCheckpoint, AllocationError> ScratchArena::try_checkpoint() noexcept
{
    if (!owner_thread()) { return std::unexpected(AllocationError::wrong_thread); }
    if (upstream_.state() != ResourceState::open) { return std::unexpected(AllocationError::closing); }
    if (sequence_ == (std::numeric_limits<std::uint64_t>::max)() || stats_.active_scopes == size_limit) {
        return std::unexpected(AllocationError::size_overflow);
    }
    ScratchCheckpoint result;
    result.arena_ = identity_; result.generation_ = stats_.generation;
    result.sequence_ = ++sequence_; result.previous_ = top_;
    result.depth_ = ++stats_.active_scopes;
    result.chunk_ = active_; result.offset_ = active_ ? active_->offset : 0;
    result.used_ = stats_.used_bytes; result.requested_ = stats_.requested_bytes;
    top_ = result.sequence_;
    sample();
    return result;
}
std::expected<void, AllocationError> ScratchArena::try_rewind(const ScratchCheckpoint& point) noexcept
{
    if (!owner_thread()) { return std::unexpected(AllocationError::wrong_thread); }
    if (point.arena_ != identity_ || point.generation_ != stats_.generation || point.sequence_ != top_
        || point.depth_ != stats_.active_scopes || stats_.active_scopes == 0) {
        return std::unexpected(AllocationError::invalid_checkpoint);
    }
    DK_PROFILE_ZONE("Memory.Scratch.Rewind");
    sample(); // Preserve the high-water observation before reclaiming.
    while (active_ != point.chunk_) {
        auto* chunk = active_;
        active_ = active_->next;
        recycle(chunk);
    }
    if (active_) { active_->offset = point.offset_; }
    stats_.used_bytes = point.used_; stats_.requested_bytes = point.requested_;
    --stats_.active_scopes;
    top_ = point.previous_;
    sample();
    return {};
}
void ScratchArena::release(Chunk* chunk) noexcept
{
    stats_.backing_bytes -= chunk->storage.size();
    --stats_.chunk_count;
    delete chunk;
}
void ScratchArena::recycle(Chunk* chunk) noexcept
{
    const auto capacity = chunk->storage.size();
    if (capacity == options_.chunk_bytes && capacity <= options_.max_retained_bytes - stats_.retained_bytes) {
        chunk->offset = 0; chunk->next = cached_; cached_ = chunk;
        stats_.retained_bytes += capacity;
    } else { release(chunk); }
}
void ScratchArena::clear_cache() noexcept
{
    while (cached_) {
        auto* chunk = cached_; cached_ = cached_->next;
        release(chunk);
    }
    stats_.retained_bytes = 0;
}
std::expected<void, AllocationError> ScratchArena::try_reset() noexcept
{
    if (!owner_thread()) { return std::unexpected(AllocationError::wrong_thread); }
    if (stats_.active_scopes != 0) { return std::unexpected(AllocationError::busy); }
    if (stats_.generation == (std::numeric_limits<std::uint64_t>::max)()) {
        return std::unexpected(AllocationError::size_overflow);
    }
    DK_PROFILE_ZONE("Memory.Scratch.Reset");
    clear_cache();
    ++stats_.generation;
    sample();
    return {};
}
std::expected<void*, AllocationError> ScratchArena::try_allocate(std::size_t bytes, std::size_t alignment) noexcept
{
    // Rejected foreign-thread calls must not even write diagnostic counters.
    if (!owner_thread()) { return std::unexpected(AllocationError::wrong_thread); }
    const auto fail = [this](AllocationError error) -> std::expected<void*, AllocationError> {
        ++stats_.failure_count; return std::unexpected(error);
    };
    if (upstream_.state() != ResourceState::open) { return fail(AllocationError::closing); }
    if (stats_.active_scopes == 0) { return fail(AllocationError::missing_scope); }
    if (!std::has_single_bit(alignment)) { return fail(AllocationError::invalid_alignment); }
    bytes = bytes == 0 ? 1 : bytes;
    if (alignment > size_limit || bytes > size_limit - (alignment - 1)
        || bytes > size_limit - stats_.requested_bytes) { return fail(AllocationError::size_overflow); }
    const auto commit = [this, bytes](Chunk* chunk, void* pointer) {
        const auto offset = static_cast<std::size_t>(static_cast<std::byte*>(pointer) - chunk->storage.data()) + bytes;
        stats_.used_bytes += offset - chunk->offset;
        stats_.requested_bytes += bytes;
        chunk->offset = offset;
        stats_.peak_used_bytes = (std::max)(stats_.peak_used_bytes, stats_.used_bytes);
        ++stats_.allocation_count;
    };
    if (active_) {
        if (auto* pointer = active_->fit(bytes, alignment)) {
            const auto consumed = static_cast<std::size_t>(static_cast<std::byte*>(pointer) - active_->storage.data()) + bytes - active_->offset;
            if (consumed > size_limit - stats_.used_bytes) { return fail(AllocationError::size_overflow); }
            commit(active_, pointer);
            return pointer;
        }
    }
    for (auto** link = &cached_; *link; link = &(*link)->next) {
        auto* chunk = *link;
        if (auto* pointer = chunk->fit(bytes, alignment)) {
            const auto consumed = static_cast<std::size_t>(static_cast<std::byte*>(pointer) - chunk->storage.data()) + bytes;
            if (consumed > size_limit - stats_.used_bytes) { return fail(AllocationError::size_overflow); }
            *link = chunk->next;
            stats_.retained_bytes -= chunk->storage.size();
            chunk->next = active_; active_ = chunk;
            commit(chunk, pointer);
            sample();
            return pointer;
        }
    }
    const auto capacity = (std::max)(options_.chunk_bytes, bytes);
    DK_PROFILE_ZONE("Memory.Scratch.Grow");
    const auto backing_alignment = (std::max)(alignof(std::max_align_t), alignment);
    if (capacity > size_limit - (backing_alignment - 1) || capacity > size_limit - stats_.backing_bytes
        || bytes > size_limit - stats_.used_bytes) { return fail(AllocationError::size_overflow); }
    auto candidate = std::unique_ptr<Chunk>{new (std::nothrow) Chunk};
    if (!candidate) { return fail(AllocationError::out_of_memory); }
    auto storage = memory::try_allocate(upstream_, capacity, backing_alignment);
    if (!storage) { return fail(storage.error()); }
    candidate->storage = std::move(*storage);
    auto* pointer = candidate->storage.data(); // Backing alignment satisfies this request exactly.
    candidate->next = active_;
    active_ = candidate.release(); // Commit: no fallible operations remain.
    stats_.backing_bytes += capacity; ++stats_.chunk_count;
    stats_.peak_backing_bytes = (std::max)(stats_.peak_backing_bytes, stats_.backing_bytes);
    commit(active_, pointer);
    sample();
    return pointer;
}
void* ScratchArena::do_allocate(std::size_t bytes, std::size_t alignment)
{
    auto result = try_allocate(bytes, alignment);
    if (!result) { throw std::bad_alloc{}; }
    return *result;
}
void ScratchArena::do_deallocate(void*, std::size_t, std::size_t)
{
    if (!owner_thread()) { std::terminate(); }
    // Object destruction is the caller's responsibility; storage lives to rewind.
}
void ScratchArena::sample() noexcept
{
    if (!owner_thread()) { std::terminate(); }
    profiling::record_scratch_sample({sampled_used_, sampled_retained_, sampled_backing_},
        {stats_.used_bytes, stats_.retained_bytes, stats_.backing_bytes});
    sampled_used_ = stats_.used_bytes; sampled_retained_ = stats_.retained_bytes; sampled_backing_ = stats_.backing_bytes;
}
} // namespace dk::memory

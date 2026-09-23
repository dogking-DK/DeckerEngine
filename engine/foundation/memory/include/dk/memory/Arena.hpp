#pragma once
#include <dk/memory/Resource.hpp>
#include <exception>
#include <thread>
#include <vector>

namespace dk::memory {
namespace detail { struct RoutingFrame; }

struct ScratchOptions {
    std::size_t chunk_bytes = 64 * 1024;
    std::size_t max_retained_bytes = 1024 * 1024;
};
struct ScratchSnapshot {
    std::size_t requested_bytes = 0;
    std::size_t used_bytes = 0; // Cursor consumption, including alignment padding.
    std::size_t backing_bytes = 0;
    std::size_t retained_bytes = 0; // Completely idle chunks only, not active tails.
    std::size_t peak_used_bytes = 0;
    std::size_t peak_backing_bytes = 0;
    std::size_t chunk_count = 0;
    std::size_t active_scopes = 0;
    std::uint64_t generation = 1;
    std::uint64_t allocation_count = 0;
    std::uint64_t failure_count = 0; // Failed allocation attempts on the owner thread.
};
class ScratchError final : public std::exception {
public:
    explicit ScratchError(AllocationError code) noexcept : code_(code) {}
    [[nodiscard]] AllocationError code() const noexcept { return code_; }
    [[nodiscard]] const char* what() const noexcept override { return "Scratch arena operation failed"; }
private:
    AllocationError code_;
};

// Opaque, single-use LIFO token. Copying does not grant another rewind.
class ScratchCheckpoint {
public:
    ScratchCheckpoint() noexcept = default;
private:
    friend class ScratchArena;
    std::uint64_t arena_ = 0, generation_ = 0, sequence_ = 0, previous_ = 0;
    std::size_t depth_ = 0, offset_ = 0, used_ = 0, requested_ = 0;
    void* chunk_ = nullptr;
};

// Thread-affine borrowed PMR. All objects must die before their checkpoint rewinds.
class ScratchArena final : public std::pmr::memory_resource {
public:
    explicit ScratchArena(ResourceHandle upstream, ScratchOptions options = {});
    ~ScratchArena() override;
    ScratchArena(const ScratchArena&) = delete;
    ScratchArena& operator=(const ScratchArena&) = delete;
    ScratchArena(ScratchArena&&) = delete;
    ScratchArena& operator=(ScratchArena&&) = delete;
    [[nodiscard]] std::expected<ScratchCheckpoint, AllocationError> try_checkpoint() noexcept;
    [[nodiscard]] std::expected<void, AllocationError> try_rewind(const ScratchCheckpoint&) noexcept;
    [[nodiscard]] std::expected<void, AllocationError> try_reset() noexcept;
    [[nodiscard]] std::expected<void*, AllocationError>
    try_allocate(std::size_t bytes, std::size_t alignment = alignof(std::max_align_t)) noexcept;
    [[nodiscard]] ScratchSnapshot snapshot() const;
    [[nodiscard]] const ResourceHandle& upstream() const noexcept { return upstream_; }
    [[nodiscard]] std::pmr::memory_resource* pmr_resource() noexcept { return this; }
    // Owner-thread safe point; no per-suballocation profiling event or global lock.
    void sample() noexcept;
private:
    struct Chunk;
    [[nodiscard]] bool owner_thread() const noexcept;
    void release(Chunk*) noexcept;
    void recycle(Chunk*) noexcept;
    void clear_cache() noexcept;
    void* do_allocate(std::size_t, std::size_t) override;
    void do_deallocate(void*, std::size_t, std::size_t) override;
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override { return this == &other; }
    ResourceHandle upstream_;
    ScratchOptions options_;
    std::thread::id thread_;
    std::uint64_t identity_ = 0, sequence_ = 0, top_ = 0;
    Chunk* active_ = nullptr;
    Chunk* cached_ = nullptr;
    ScratchSnapshot stats_;
    std::size_t sampled_used_ = 0, sampled_retained_ = 0, sampled_backing_ = 0;
};

class ScratchScope {
public:
    ScratchScope(); // Current ThreadContext; publishes a scratch route in this execution frame.
    explicit ScratchScope(ScratchArena& arena); // Does not alter TLS.
    ~ScratchScope();
    ScratchScope(const ScratchScope&) = delete;
    ScratchScope& operator=(const ScratchScope&) = delete;
    ScratchScope(ScratchScope&&) = delete;
    ScratchScope& operator=(ScratchScope&&) = delete;
    [[nodiscard]] std::pmr::memory_resource* resource() const noexcept { return arena_->pmr_resource(); }
private:
    ScratchArena* arena_;
    ScratchCheckpoint checkpoint_;
    detail::RoutingFrame* frame_ = nullptr;
    ScratchScope* previous_ = nullptr;
};

[[nodiscard]] std::pmr::memory_resource* current_scratch_resource();
template<class T> [[nodiscard]] std::pmr::vector<T> scratch_vector()
{ return std::pmr::vector<T>{current_scratch_resource()}; }
} // namespace dk::memory

#pragma once
#include <dk/memory/Resource.hpp>
#include <new>

namespace dk::memory {
namespace detail { class PoolControl; }
using PoolOptions = std::pmr::pool_options;
struct PoolSnapshot {
    ResourceState state = ResourceState::closed;
    std::size_t logical_live_bytes = 0;
    std::size_t live_allocations = 0;
    std::size_t active_operations = 0;
    std::size_t backing_bytes = 0;
    // All backing only when empty/quiescent; not an estimate of free slots in a live pool.
    std::size_t idle_backing_bytes = 0;
    std::size_t peak_logical_bytes = 0;
    std::size_t peak_backing_bytes = 0;
    std::uint64_t allocation_count = 0;
    std::uint64_t failure_count = 0;
    std::uint64_t backing_allocation_count = 0;
    std::uint64_t total_backing_bytes = 0;
};
class PoolError final : public std::bad_alloc {
public:
    explicit PoolError(AllocationError code) noexcept : code_(code) {}
    [[nodiscard]] AllocationError code() const noexcept { return code_; }
    [[nodiscard]] const char* what() const noexcept override { return "Pool resource operation failed"; }
private:
    AllocationError code_;
};

// Stable, thread-affine owner. PMR and object deleters borrow it.
class LocalPoolResource {
public:
    explicit LocalPoolResource(ResourceHandle upstream, PoolOptions options = {});
    ~LocalPoolResource();
    LocalPoolResource(const LocalPoolResource&) = delete;
    LocalPoolResource& operator=(const LocalPoolResource&) = delete;
    LocalPoolResource(LocalPoolResource&&) = delete;
    LocalPoolResource& operator=(LocalPoolResource&&) = delete;
    [[nodiscard]] bool on_owner_thread() const noexcept;
    [[nodiscard]] ResourceHandle upstream() const noexcept;
    [[nodiscard]] PoolOptions options() const noexcept;
    [[nodiscard]] ResourceState state() const noexcept;
    [[nodiscard]] PoolSnapshot snapshot() const;
    [[nodiscard]] std::pmr::memory_resource* pmr_resource() noexcept;
    [[nodiscard]] std::expected<void*, AllocationError> try_allocate(std::size_t bytes,
        std::size_t alignment = alignof(std::max_align_t)) noexcept;
    void deallocate(void*, std::size_t bytes, std::size_t alignment = alignof(std::max_align_t)) noexcept;
    [[nodiscard]] std::expected<void, AllocationError> try_trim() noexcept;
    [[nodiscard]] std::expected<void, AllocationError> try_sample() noexcept;
    void begin_close() noexcept;
    [[nodiscard]] std::expected<CloseResult, AllocationError> try_close() noexcept;
private:
    std::unique_ptr<detail::PoolControl> control_;
};

// Copyable owner of a stable synchronized PMR. Keep an owner through every raw/PMR free.
// Different copies can be used concurrently; assignment/destruction of one copy is exclusive.
class SharedPoolResource {
public:
    SharedPoolResource() noexcept = default;
    [[nodiscard]] static std::expected<SharedPoolResource, AllocationError>
    create(ResourceHandle upstream, PoolOptions options = {}) noexcept;
    [[nodiscard]] explicit operator bool() const noexcept { return bool(control_); }
    [[nodiscard]] bool operator==(const SharedPoolResource&) const noexcept = default;
    [[nodiscard]] ResourceHandle upstream() const noexcept;
    [[nodiscard]] PoolOptions options() const noexcept;
    [[nodiscard]] ResourceState state() const noexcept;
    [[nodiscard]] PoolSnapshot snapshot() const noexcept;
    [[nodiscard]] std::pmr::memory_resource* pmr_resource() const noexcept;
    [[nodiscard]] std::expected<void*, AllocationError> try_allocate(std::size_t bytes,
        std::size_t alignment = alignof(std::max_align_t)) const noexcept;
    void deallocate(void*, std::size_t bytes, std::size_t alignment = alignof(std::max_align_t)) const noexcept;
    [[nodiscard]] std::expected<void, AllocationError> try_trim() const noexcept;
    [[nodiscard]] std::expected<void, AllocationError> try_sample() const noexcept;
    void begin_close() const noexcept;
    [[nodiscard]] CloseResult try_close() const noexcept;
private:
    std::shared_ptr<detail::PoolControl> control_;
};
} // namespace dk::memory

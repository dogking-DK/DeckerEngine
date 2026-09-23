#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <memory_resource>
#include <string_view>

namespace dk::memory {

using SystemId = std::uint64_t;
using DomainId = std::uint64_t;
enum class ResourceState { open, closing, closed };
enum class DomainCategory { general, assets, scene, render, jobs, other };
enum class AllocationError {
    invalid_handle, invalid_argument, invalid_alignment, size_overflow,
    limit_exceeded, out_of_memory, closing, wrong_thread, missing_scope, invalid_checkpoint, busy
};

struct HeapOptions {
    std::string_view name = "general";
    DomainCategory category = DomainCategory::general;
    std::size_t budget_bytes = 0; // Zero means unlimited normalized request bytes.
};

struct ResourceSnapshot {
    SystemId system_id = 0;
    DomainId domain_id = 0;
    ResourceState state = ResourceState::closed;
    std::size_t budget_bytes = 0;
    std::size_t reserved_bytes = 0; // Committed + in-flight requests.
    std::size_t backing_requested_bytes = 0;
    std::size_t peak_backing_bytes = 0;
    std::size_t live_allocations = 0;
    std::size_t active_operations = 0;
    std::uint64_t allocation_count = 0;
    std::uint64_t failure_count = 0;
};

struct CloseResult {
    ResourceState state = ResourceState::closed;
    std::size_t live_allocations = 0;
    std::size_t active_operations = 0;
    std::size_t backing_requested_bytes = 0;
    std::size_t active_contexts = 0;
    [[nodiscard]] bool closed() const noexcept { return state == ResourceState::closed; }
};

// Multiplication only; try_allocate also checks aligned allocation size limits.
[[nodiscard]] std::expected<std::size_t, AllocationError>
checked_byte_size(std::size_t count, std::size_t element_size) noexcept;

namespace detail { struct ResourceControl; }
class MemorySystem;

class ResourceHandle {
public:
    ResourceHandle() noexcept = default;
    [[nodiscard]] explicit operator bool() const noexcept { return bool(control_); }
    [[nodiscard]] bool operator==(const ResourceHandle&) const noexcept = default;
    [[nodiscard]] SystemId system_id() const noexcept;
    [[nodiscard]] DomainId domain_id() const noexcept;
    [[nodiscard]] std::string_view name() const noexcept; // Borrowed until the last handle dies.
    [[nodiscard]] DomainCategory category() const noexcept;
    [[nodiscard]] ResourceState state() const noexcept;
    [[nodiscard]] ResourceSnapshot snapshot() const noexcept; // Non-transactional atomic sample.
    // Borrowed; retain an owner until every PMR user is destroyed. Empty -> null_memory_resource.
    [[nodiscard]] std::pmr::memory_resource* pmr_resource() const noexcept;

    // Keep an owning handle alive through deallocation. Zero bytes consume one byte.
    [[nodiscard]] std::expected<void*, AllocationError>
    try_allocate(std::size_t bytes, std::size_t alignment = alignof(std::max_align_t)) const noexcept;
    // Same resource and original bytes/alignment are required. Null is a no-op.
    void deallocate(void* pointer, std::size_t bytes,
                    std::size_t alignment = alignof(std::max_align_t)) const noexcept;
    void begin_close() const noexcept;
    [[nodiscard]] CloseResult try_close() const noexcept;

private:
    friend class MemorySystem;
    explicit ResourceHandle(std::shared_ptr<detail::ResourceControl> control) noexcept;
    std::shared_ptr<detail::ResourceControl> control_;
};

} // namespace dk::memory

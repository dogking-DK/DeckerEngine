#pragma once
#include <dk/memory/MemorySystem.hpp>

namespace dk::memory::detail {

// Internal deterministic fault/ordering seam, not a supported public allocator API.
// Every callback is noexcept; context must outlive all systems, handles and calls.
struct Backend {
    void* context = nullptr;
    void* (*create)(void*) noexcept = nullptr;
    void (*destroy)(void*, void*) noexcept = nullptr;
    void* (*allocate)(void*, void*, std::size_t, std::size_t) noexcept = nullptr;
    void (*free)(void*, void*, void*, std::size_t, std::size_t) noexcept = nullptr;
};
struct EventSink {
    void* context = nullptr;
    void (*allocate)(void*, const void*, std::size_t, DomainCategory) noexcept = nullptr;
    void (*free)(void*, const void*, DomainCategory) noexcept = nullptr;
};
[[nodiscard]] Backend mimalloc_backend() noexcept;
struct MemoryAccess {
    static std::expected<MemorySystem, AllocationError> create(Backend, EventSink) noexcept;
};
} // namespace dk::memory::detail

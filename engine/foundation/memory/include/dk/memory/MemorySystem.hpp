#pragma once
#include <dk/memory/Resource.hpp>

namespace dk::memory {
namespace detail { struct MemoryAccess; }

// Methods are thread-safe; moving/destroying this wrapper requires exclusive access.
// Destruction starts closing. External handles retain their heaps for legal frees.
class MemorySystem {
public:
    [[nodiscard]] static std::expected<MemorySystem, AllocationError> create() noexcept;
    ~MemorySystem();
    MemorySystem(MemorySystem&&) noexcept;
    MemorySystem& operator=(MemorySystem&&) noexcept;
    MemorySystem(const MemorySystem&) = delete;
    MemorySystem& operator=(const MemorySystem&) = delete;

    [[nodiscard]] SystemId id() const noexcept;
    [[nodiscard]] ResourceState state() const noexcept;
    [[nodiscard]] std::expected<ResourceHandle, AllocationError> create_heap(HeapOptions options = {}) noexcept;
    void begin_close() noexcept;
    [[nodiscard]] CloseResult try_close() noexcept;

private:
    friend struct detail::MemoryAccess;
    struct Impl;
    explicit MemorySystem(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};
} // namespace dk::memory
